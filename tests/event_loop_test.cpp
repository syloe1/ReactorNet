#include <gtest/gtest.h>

#include "EventLoop.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

// Unit tests for EventLoop's cross-thread dispatch — pendingFunctors_ + mutex_,
// the eventfd wakeup, and doPendingFunctors()'s swap semantics.
//
// Every failure mode here is silent rather than loud. Comment out the wakeup()
// in queueInLoop() and every cross-thread post still arrives with the right
// value on the right thread — just up to kPollTimeMs (10s in EventLoop.cpp)
// late, whenever the loop's poll happens to time out on its own. Nothing errors,
// nothing crashes, only the *latency* changes. So the tests that drive a running
// loop assert against an explicit budget far below that 10s and treat "not done
// yet" as a failure; the budget is the whole point, since a plain "eventually
// true" assertion would pass either way.
//
// The file is in two halves. The first half needs no running loop at all — an
// EventLoop can be built and destroyed on the test thread, which is also what
// makes runInLoop()'s in-thread branch directly observable. The second half owns
// a thread whose only job is to run loop().
namespace {

using namespace std::chrono_literals;

// Backstop for every round-trip: a post that is not observed within 3s is a hang,
// not slowness. Legitimate latencies in this file are microseconds.
constexpr auto kWaitTimeout = 3s;

// Backstop for the fixture's startup handshake only, and deliberately above
// kPollTimeMs (10s in EventLoop.cpp).
//
// It is the one wait in this file that has to survive a *missing wakeup*: with
// the wakeup removed the loop still gets there, just one poll timeout later, and
// a 3s backstop would make SetUp fail instead. That would be a fixture error
// rather than the wakeup case's own timeout failure — exactly the evidence the
// reverse verification is supposed to produce, so it must not be pre-empted.
constexpr auto kStartupTimeout = 12s;

// The budget for the wakeup case, and the number that makes it meaningful.
// kPollTimeMs is 10s, so a loop that is *not* woken still runs the functor — a
// full 10s later. 1s sits far above any real wakeup latency (µs) and far below
// that timeout, so the case can only pass by actually being woken. When it
// fails, it fails at ~1s rather than ~10s, which is what tells the two apart.
constexpr auto kWakeupBudget = 1s;

// Collects callback invocations from the loop thread and lets the test thread
// wait for them, with the two facts these tests care about: how many times, and
// whether the callback found itself on the loop thread.
//
// Always held by shared_ptr and captured *by value* in the posted functor —
// never captured by reference to a test-body local. When a wait times out (the
// failure this file exists to detect) the test body returns and its locals die
// while the functor is still queued; TearDown's quit() then wakes the loop,
// whose final iteration drains the queue. A by-reference capture would therefore
// hand the loop thread a destroyed Probe at exactly the moment the suite is
// trying to report a clean timeout — the timeout would be reported and then the
// process would die of use-after-free (seen as a glibc abort in
// __pthread_tpp_change_priority) instead of failing cleanly.
class Probe {
public:
  void run(bool inLoopThread) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runs_ == 0) {
      firstRunAt_ = std::chrono::steady_clock::now();
      inLoopThread_ = inLoopThread;
    }
    ++runs_;
    cond_.notify_all();
  }

  // Predicate form, so a spurious wakeup just re-checks. Returns false on
  // timeout — the caller asserts on it, because "did not arrive in time" is a
  // failure this suite must be able to report rather than hang on.
  bool waitFor(int count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cond_.wait_for(lock, timeout, [&] { return runs_ >= count; });
  }

  int runs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runs_;
  }

  // Only meaningful after waitFor() has succeeded.
  bool ranInLoopThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inLoopThread_;
  }

  std::chrono::steady_clock::time_point firstRunAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return firstRunAt_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  int runs_ = 0;
  bool inLoopThread_ = false;
  std::chrono::steady_clock::time_point firstRunAt_{};
};

// --- Thread affinity and same-thread dispatch, with no loop running ----------
//
// An EventLoop is usable without loop(): the constructor already built its
// Poller, its wakeup Channel and its TimerQueue, and the destructor tears them
// down. So the thread that constructs one is "the loop thread" as far as
// isInLoopThread() and runInLoop() are concerned, and the test thread can play
// that role directly. That makes the in-thread branch observable without any
// synchronization at all: if runInLoop() queued instead of calling, the callback
// could never run, because loop() is never entered here.

// Regression test for ~EventLoop().
//
// The destructor used to `delete poller_` inside its body, but timerQueue_ is a
// unique_ptr *member*, and members are destroyed after the body has run. So
// ~TimerQueue went second: it disables its timerfd Channel, Channel::update()
// forwards that to EventLoop::updateChannel(), and that called through the
// already-freed poller_. Every EventLoop that was ever destroyed segfaulted —
// which also means every EventLoopThread, i.e. TcpServer's IO thread pool, took
// the whole process down when it was torn down.
//
// The rest of this file depends on the same teardown path, so this stays first.
TEST(EventLoopTest, DestroyingALoopDoesNotCrash) {
  // A loop that never armed a timer is enough to reach it: TimerQueue registers
  // its timerfd Channel unconditionally, in its constructor.
  //
  // Own thread, because an EventLoop belongs to the thread that created it, and
  // a __thread guard in EventLoop.cpp aborts outright if a second EventLoop
  // appears in the same thread.
  std::thread thread([] { EventLoop loop; });
  thread.join();
}

TEST(EventLoopTest, LoopThreadIdentityIsVisibleOnTheLoopThread) {
  EventLoop loop; // this thread becomes the loop thread

  EXPECT_TRUE(loop.isInLoopThread());
  // getEventLoopOfCurrentThread() reads the __thread pointer the constructor
  // installed, so on the constructing thread it must hand back this very loop.
  EXPECT_EQ(EventLoop::getEventLoopOfCurrentThread(), &loop);
}

TEST(EventLoopTest, IsInLoopThreadIsFalseFromAnotherThread) {
  EventLoop loop;
  ASSERT_TRUE(loop.isInLoopThread());

  // The check has to happen *in* the other thread — asking from here would just
  // re-answer for this one, which is the thread that owns the loop.
  std::atomic<bool> sawItselfAsTheLoopThread{true};
  std::thread other([&] { sawItselfAsTheLoopThread = loop.isInLoopThread(); });
  other.join();

  EXPECT_FALSE(sawItselfAsTheLoopThread.load())
      << "a loop belongs to the thread that constructed it; every other thread "
         "must see isInLoopThread() == false, which is what routes runInLoop() "
         "down the queue-instead-of-call branch";
}

TEST(EventLoopTest, GetEventLoopOfCurrentThreadIsNullOutsideTheLoopThread) {
  // The test thread has a loop, so nullptr in the other thread cannot be
  // explained by "no loop exists anywhere" — it is the thread-locality itself.
  EventLoop loop;
  ASSERT_EQ(EventLoop::getEventLoopOfCurrentThread(), &loop);

  std::atomic<bool> sawALoop{true};
  std::thread other([&] {
    sawALoop = EventLoop::getEventLoopOfCurrentThread() != nullptr;
  });
  other.join();

  EXPECT_FALSE(sawALoop.load())
      << "the __thread pointer must not leak across threads: a second thread "
         "has no loop of its own yet, so it must read nullptr";
}

TEST(EventLoopTest, RunInLoopRunsInlineOnTheLoopThread) {
  EventLoop loop;

  bool ran = false;
  loop.runInLoop([&] { ran = true; });

  // Asserted before loop() has ever run an iteration — it is never called in
  // this test at all. So a true here can only mean the callback was invoked
  // directly by runInLoop(), on the calling thread. Had it gone through
  // pendingFunctors_ instead, only doPendingFunctors() could run it, and nothing
  // in this test will ever drain the queue.
  EXPECT_TRUE(ran) << "runInLoop() on the loop thread must call through "
                      "immediately, not enqueue and wait for a loop iteration";
}

// --- Driving loop() from another thread -------------------------------------

class EventLoopLoopTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto started = std::make_shared<std::promise<EventLoop *>>();
    std::future<EventLoop *> ready = started->get_future();

    // Shared rather than captured by reference: the promise must outlive
    // SetUp()'s stack frame, which it does only until ready.get() returns.
    thread_ = std::thread([started, this] {
      // Built here, in the thread that will run it — the constructor asserts
      // loop-thread affinity all over (Poller, Channels, TimerQueue), and the
      // __thread guard aborts on a second EventLoop in one thread.
      EventLoop loop;
      started->set_value(&loop);
      loop.loop();
      // Only reachable once loop() has returned, which is what the quit() case
      // waits on. The fixture outlives the thread (TearDown joins first), so
      // touching the member here is safe.
      loopReturned_.set_value();
    });

    loop_ = ready.get();

    // Handshake: functors only run from inside loop()'s body, so observing one
    // proves loop() has been entered. That matters twice over.
    //
    // First, loop() opens with `quit_ = false`, which silently swallows a quit()
    // issued before it gets there; without this handshake a fast-failing test
    // could have its TearDown quit() discarded and then hang in join() forever.
    // Second, it leaves the loop parked in poll, so a latency measured after it
    // is a real "blocked → woken" measurement.
    //
    // Shared for the same reason Probe is: if this handshake ever times out, the
    // functor is still queued when SetUp's frame goes away, and the loop thread
    // would later set_value() on a destroyed promise.
    auto entered = std::make_shared<std::promise<void>>();
    std::future<void> enteredFuture = entered->get_future();
    loop_->runInLoop([entered] { entered->set_value(); });

    // wait_for rather than get(): a loop that failed to start must fail the test
    // rather than block it.
    ASSERT_EQ(enteredFuture.wait_for(kStartupTimeout), std::future_status::ready)
        << "loop() never reached its first iteration";
  }

  void TearDown() override {
    // Guarded: if SetUp() failed before publishing the loop, there is nothing to
    // quit — but the thread may still be there to join.
    if (loop_ != nullptr) {
      loop_->quit(); // thread-safe: sets the flag, then wakes the loop
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    // The loop thread unwinds EventLoop itself after loop() returns; destroying
    // it from here would trip the affinity assert.
  }

  EventLoop *loop_ = nullptr;
  std::thread thread_;
  // Set by the loop thread after loop() returns. get_future() is called by the
  // one case that waits on it.
  std::promise<void> loopReturned_;
};

TEST_F(EventLoopLoopTest, RunInLoopFromAnotherThreadExecutesOnTheLoopThread) {
  auto probe = std::make_shared<Probe>();

  loop_->runInLoop([probe, this] { probe->run(loop_->isInLoopThread()); });

  ASSERT_TRUE(probe->waitFor(1, kWaitTimeout))
      << "a cross-thread post must reach the loop thread";
  // The point of the case: the callback ran, and it ran over there. Asserted from
  // the test thread rather than inside the callback so a failure is reported
  // where the wait is, not from a foreign thread.
  EXPECT_TRUE(probe->ranInLoopThread())
      << "the whole purpose of the queue is to move the call onto the loop "
         "thread; running it on the caller's thread would be a silent break of "
         "every single-threaded assumption downstream";
  EXPECT_EQ(probe->runs(), 1) << "exactly once, not once per iteration";
}

// queueInLoop() must not shortcut, even when the caller is already the loop
// thread — otherwise a callback could re-enter library code in the middle of
// doPendingFunctors() instead of after it.
//
// This case also guards the second disjunct of queueInLoop()'s wakeup condition
// (`|| callingPendingFunctors_`), which nothing else here covers. The inner
// functor is queued from *inside* an outer one, so the loop is not blocked in
// poll at the time and would otherwise sit on it until the 10s poll timeout.
TEST_F(EventLoopLoopTest, QueueInLoopDefersEvenOnTheLoopThread) {
  auto inner = std::make_shared<Probe>();
  // pessimism: overwritten by the loop thread
  auto ranInline = std::make_shared<std::atomic<bool>>(true);

  loop_->runInLoop([inner, ranInline, this] {
    loop_->queueInLoop([inner, this] { inner->run(loop_->isInLoopThread()); });
    // Read immediately, still on the loop thread and inside doPendingFunctors():
    // doPendingFunctors() swapped the queue into a local, so this functor landed
    // in the member queue and cannot run until the next iteration.
    ranInline->store(inner->runs() != 0);
  });

  // Bounded, and deliberately below the poll timeout: reaching the next
  // iteration requires the wakeup that the callingPendingFunctors_ disjunct
  // arms, so a missing wakeup shows up here as a timeout.
  ASSERT_TRUE(inner->waitFor(1, kWakeupBudget))
      << "a functor queued from the loop thread must be run on the next "
         "iteration, woken by the eventfd — not after the poll times out";
  EXPECT_FALSE(ranInline->load())
      << "queueInLoop() must never execute inline, even on the loop thread";
}

TEST_F(EventLoopLoopTest, CrossThreadPostIsWokenNotLeftToThePollTimeout) {
  auto probe = std::make_shared<Probe>();

  // SetUp's handshake left the loop back in poll with nothing armed, so this
  // sleep is not synchronization: the post lands on a parked loop either way,
  // and both interleavings pass. It just keeps the reported latency an honest
  // park → wake measurement.
  std::this_thread::sleep_for(20ms);

  const auto postedAt = std::chrono::steady_clock::now();
  loop_->runInLoop([probe, this] { probe->run(loop_->isInLoopThread()); });

  const bool woken = probe->waitFor(1, kWakeupBudget);
  const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - postedAt);

  // The failure mode of this case is the timeout itself, so report the number
  // that was actually observed, not just a boolean.
  ASSERT_TRUE(woken) << "a cross-thread post must be woken by the eventfd; "
                        "still nothing after "
                     << waited.count()
                     << "us, while the poll timeout sits at 10s — the wakeup is "
                        "what makes the difference between this budget and that "
                        "timeout";

  const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
      probe->firstRunAt() - postedAt);
  RecordProperty("wakeup_latency_us", static_cast<int>(latency.count()));
  EXPECT_TRUE(probe->ranInLoopThread());
  EXPECT_LT(latency, kWakeupBudget);
}

TEST_F(EventLoopLoopTest, QuitMakesLoopReturn) {
  // Must be fetched before the loop thread can set it — which it may already
  // have done for all we know, and that is fine: a promise can be satisfied
  // before get_future() is ever called, it just cannot be retrieved twice.
  std::future<void> returned = loopReturned_.get_future();

  // No handshake needed here: SetUp's already proved loop() is running, so this
  // quit() cannot be the one that loop()'s opening `quit_ = false` discards.
  loop_->quit();

  EXPECT_EQ(returned.wait_for(kWaitTimeout), std::future_status::ready)
      << "quit() must break loop() out of poll and let it return";
  // The join is TearDown's: doing it here too would join a thread twice.
}

} // namespace
