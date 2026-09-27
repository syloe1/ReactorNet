#include <gtest/gtest.h>

#include "EventLoop.h"
#include "Timer.h"
#include "TimerQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Unit tests for TimerQueue — the min-heap + timerfd pairing.
//
// Two properties here are invisible to anyone reading the code, and both fail
// silently rather than loudly:
//
//   1. timers_ is a *min*-heap built on std::push_heap/pop_heap, which default
//      to a max-heap. Timer::TimerPtrComparator inverts the comparison to
//      cancel that out. Flip the comparison back and every timer still fires,
//      just in the wrong order, so only a test that watches the order catches
//      it.
//
//   2. cancel() has to keep two structures in sync — the heap (timers_) and the
//      activeTimers_ set. Drift between them compiles and runs; the damage is
//      a callback that fires when it should not, or a pointer freed twice.
//
// Driving the loop: EventLoop builds its Poller, its Channels and its
// TimerQueue eagerly and asserts thread affinity throughout, and a __thread
// guard in EventLoop.cpp aborts outright if a second EventLoop appears in the
// same thread. So the fixture owns a thread and builds both objects inside it,
// handing the main thread nothing but pointers. Every timer is posted from the
// main thread through addTimer/cancel, both of which route through
// EventLoop::runInLoop and are therefore safe to call from off-loop.
namespace {

using namespace std::chrono_literals;

// Backstop for every wait. Delays in this file are 20–200ms, so a wait that is
// still unsatisfied after 3s is a hang rather than slowness.
constexpr auto kWaitTimeout = 3s;

// Collects callback invocations from the loop thread and lets the main thread
// wait for a given count. Recording order is the point of several tests, so the
// ids are kept in call order rather than counted.
class Recorder {
public:
  void record(long id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ids_.push_back(id);
    cond_.notify_all();
  }

  // Predicate form, so a spurious wakeup just re-checks.
  bool waitFor(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cond_.wait_for(lock, timeout, [&] { return ids_.size() >= count; });
  }

  std::vector<long> ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ids_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  std::vector<long> ids_;
};

// --- Timer itself -----------------------------------------------------------
//
// Timer needs no loop: expiration/interval/repeat/sequence are plain state, and
// restart() is pure arithmetic. Testing them here keeps a failure pointed at
// Timer rather than at the queue that drives it.

TEST(TimerTest, RepeatIsTrueOnlyForPositiveInterval) {
  Timer oneShot([] {}, Timestamp::now(), 0.0);
  EXPECT_FALSE(oneShot.repeat());

  Timer repeating([] {}, Timestamp::now(), 0.05);
  EXPECT_TRUE(repeating.repeat());

  // interval defaults to 0, i.e. one-shot.
  Timer defaulted([] {}, Timestamp::now());
  EXPECT_FALSE(defaulted.repeat());
}

TEST(TimerTest, SequenceIsStrictlyIncreasing) {
  Timer first([] {}, Timestamp::now());
  Timer second([] {}, Timestamp::now());
  Timer third([] {}, Timestamp::now());

  EXPECT_LT(first.sequence(), second.sequence());
  EXPECT_LT(second.sequence(), third.sequence());
}

TEST(TimerTest, RestartMovesExpirationForwardByOneIntervalFromNow) {
  Timestamp tenSecondsOut = Timestamp::now();
  tenSecondsOut += 10.0;
  Timer timer([] {}, tenSecondsOut, 1.0);
  ASSERT_TRUE(timer.expiration().valid());

  const Timestamp now = Timestamp::now();
  timer.restart(now);

  // restart(now) is `expiration_ = now; expiration_ += interval_`, and
  // Timestamp::operator+= truncates 1.0 * 1e6 to exactly 1000000µs, so this is
  // an exact comparison rather than an approximate one.
  EXPECT_EQ(timer.expiration().microSecondsSinceEpoch(),
            now.microSecondsSinceEpoch() + 1000000);
  // The old expiration was 10s out, so the timer was pulled *forward* — this is
  // what makes a repeating timer fire on its interval rather than drift.
  EXPECT_LT(timer.expiration().microSecondsSinceEpoch(),
            tenSecondsOut.microSecondsSinceEpoch());
}

TEST(TimerTest, RestartInvalidatesOneShotTimer) {
  Timestamp oneSecondOut = Timestamp::now();
  oneSecondOut += 1.0;
  Timer timer([] {}, oneSecondOut, 0.0);
  ASSERT_TRUE(timer.expiration().valid());

  timer.restart(Timestamp::now());

  // For a one-shot, restart() parks the expiration at the default Timestamp().
  // reset() relies on this: it only re-inserts timers whose expiration is still
  // valid, and deletes the rest.
  EXPECT_FALSE(timer.expiration().valid());
  EXPECT_EQ(timer.expiration().microSecondsSinceEpoch(), 0);
}

// --- TimerQueue -------------------------------------------------------------

class TimerQueueTest : public ::testing::Test {
protected:
  struct LoopContext {
    EventLoop *loop;
    TimerQueue *queue;
  };

  void SetUp() override {
    auto started = std::make_shared<std::promise<LoopContext>>();
    std::future<LoopContext> ready = started->get_future();

    // Shared rather than captured by reference: the promise must outlive
    // SetUp()'s stack frame, which it does only until ready.get() returns.
    thread_ = std::thread([started] {
      // Both objects belong to this thread. TimerQueue's constructor enables a
      // Channel, and registering a Channel asserts loop-thread affinity, so
      // constructing them anywhere else aborts.
      EventLoop loop;
      TimerQueue queue(&loop);
      started->set_value(LoopContext{&loop, &queue});
      loop.loop();
    });

    const LoopContext context = ready.get();
    loop_ = context.loop;
    queue_ = context.queue;
  }

  void TearDown() override {
    loop_->quit(); // thread-safe: sets the flag and wakes the loop
    thread_.join();
    // The loop thread unwinds EventLoop and TimerQueue itself, after loop()
    // returns — destroying either from here would trip the affinity assert.
  }

  // The main thread's only way in. Both of these defer into
  // EventLoop::runInLoop, so calling them from off-loop is safe despite the
  // "must be called from the EventLoop thread" comment on addTimer. The queue
  // is FIFO, which is what makes "addTimer then immediately cancel" land as
  // insert-then-erase instead of a race.
  Timer *addTimer(std::function<void()> cb, double delay,
                  double interval = 0.0) {
    return queue_->addTimer(std::move(cb), delay, interval);
  }

  // Absolute overload: two timers sharing one Timestamp are guaranteed to be
  // collected by the same getExpired() pass.
  Timer *addTimerAt(std::function<void()> cb, Timestamp when) {
    return queue_->addTimer(std::move(cb), when, 0.0);
  }

  EventLoop *loop_ = nullptr;
  TimerQueue *queue_ = nullptr;
  std::thread thread_;
};

TEST_F(TimerQueueTest, OneShotTimerFiresExactlyOnce) {
  Recorder rec;
  std::atomic<int> runs{0};

  addTimer(
      [&] {
        ++runs;
        rec.record(1);
      },
      0.03);

  // A later sentinel timer, not a sleep: by the time it fires, the 30ms window
  // is provably over, so "exactly once" is a real assertion rather than a race
  // that happens to be won.
  addTimer([&] { rec.record(99); }, 0.12);

  ASSERT_TRUE(rec.waitFor(2, kWaitTimeout));
  EXPECT_EQ(rec.ids(), (std::vector<long>{1, 99}));
  EXPECT_EQ(runs.load(), 1) << "a one-shot timer must not fire twice";
}

TEST_F(TimerQueueTest, RepeatingTimerFiresMoreThanOnce) {
  Recorder rec;

  addTimer([&] { rec.record(1); }, 0.02, 0.02);

  // Three recordings of the same id can only come from three separate
  // invocations of one timer object.
  ASSERT_TRUE(rec.waitFor(3, kWaitTimeout))
      << "a repeating timer must fire again after each interval";
  EXPECT_EQ(rec.ids(), (std::vector<long>{1, 1, 1}));
}

// The min-heap property, tested through the public API: three timers posted
// furthest-first. A max-heap would pop them 100, 60, 20 — every timer still
// fires, so only the order distinguishes the two.
TEST_F(TimerQueueTest, FiresInExpirationOrderDespiteInsertionOrder) {
  Recorder rec;

  addTimer([&] { rec.record(100); }, 0.10);
  addTimer([&] { rec.record(20); }, 0.02);
  addTimer([&] { rec.record(60); }, 0.06);

  ASSERT_TRUE(rec.waitFor(3, kWaitTimeout));
  EXPECT_EQ(rec.ids(), (std::vector<long>{20, 60, 100}))
      << "the heap must pop by expiration, not by insertion order";
}

TEST_F(TimerQueueTest, SimultaneousExpirationsAllFire) {
  Recorder rec;

  Timestamp when = Timestamp::now();
  when += 0.05; // shared by all three, so getExpired() collects them together

  addTimerAt([&] { rec.record(1); }, when);
  addTimerAt([&] { rec.record(2); }, when);
  addTimerAt([&] { rec.record(3); }, when);

  ASSERT_TRUE(rec.waitFor(3, kWaitTimeout))
      << "every timer in the batch must run, none may be dropped";

  // TimerPtrComparator compares expirations only, so the relative order of
  // equal timestamps is unspecified. Assert the set, not the sequence.
  std::vector<long> ids = rec.ids();
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<long>{1, 2, 3}));
}

TEST_F(TimerQueueTest, ImmediateCancelPreventsCallback) {
  Recorder rec;
  std::atomic<int> runs{0};

  Timer *victim = addTimer(
      [&] {
        ++runs;
        rec.record(1);
      },
      0.04);
  queue_->cancel(victim);

  addTimer([&] { rec.record(99); }, 0.12); // sentinel: the 40ms window is past

  ASSERT_TRUE(rec.waitFor(1, kWaitTimeout));
  EXPECT_EQ(runs.load(), 0) << "a cancelled timer must never run its callback";
  EXPECT_EQ(rec.ids(), (std::vector<long>{99}));
}

// cancel() removes the timer from the heap by find+erase followed by a full
// make_heap() rebuild. Both remaining timers have to survive that rebuild.
TEST_F(TimerQueueTest, CancelKeepsHeapConsistentForRemainingTimers) {
  Recorder rec;

  addTimer([&] { rec.record(40); }, 0.04);
  Timer *middle = addTimer([&] { rec.record(80); }, 0.08);
  addTimer([&] { rec.record(120); }, 0.12);
  addTimer([&] { rec.record(99); }, 0.16); // sentinel, after the 80ms window
  queue_->cancel(middle);

  ASSERT_TRUE(rec.waitFor(3, kWaitTimeout));
  EXPECT_EQ(rec.ids(), (std::vector<long>{40, 120, 99}))
      << "the remaining timers must still fire in order, and the cancelled one "
         "must not fire at all";
}

// Cancelling a timer that has already expired but whose callback has not run
// yet — the cancellor is its batch-mate, so the victim is out of the heap but
// still in the expired list.
//
// This is the case that keeps getExpired()'s activeTimers_.erase() load-bearing.
// Drop that erase and the set still claims a timer the heap has already given
// up, so this cancel reaches the `delete` below while reset() is still holding
// the same pointer — a double free, and glibc aborts the process.
TEST_F(TimerQueueTest, CancelOfAlreadyExpiredTimerInSameBatchIsSafe) {
  Recorder rec;
  std::atomic<int> victimRuns{0};

  Timestamp when = Timestamp::now();
  when += 0.05; // one timestamp for both, so they land in one getExpired() pass

  Timer *victim = addTimerAt([&] { ++victimRuns; }, when);
  addTimerAt(
      [&] {
        queue_->cancel(victim);
        rec.record(1);
      },
      when);

  addTimer([&] { rec.record(99); }, 0.10); // sentinel: the loop survived

  ASSERT_TRUE(rec.waitFor(2, kWaitTimeout))
      << "cancelling a same-batch expired timer must not take the loop down";
  EXPECT_EQ(rec.ids(), (std::vector<long>{1, 99}));
  // Only the upper bound is asserted. The current implementation treats this
  // cancel as a no-op — callingExpiredTimers_ is set but never read — so the
  // victim usually runs once; a future fix that suppresses a pending callback
  // would legitimately make this 0. Either way it must never run twice.
  EXPECT_LE(victimRuns.load(), 1);
}

} // namespace
