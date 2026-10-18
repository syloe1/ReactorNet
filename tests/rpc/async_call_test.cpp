#include <gtest/gtest.h>

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "rpc/rpc_client.h"
#include "rpc/rpc_server.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace std::chrono_literals;

// Tests for the non-blocking entry points (#35): AsyncCall() delivers its
// callback on the loop thread, and CallAsync() returns a future that is safe
// to wait on from a business thread.
namespace {

constexpr auto kWaitTimeout = 5s;

// Shared by the test thread and the callbacks. It is a shared_ptr on purpose:
// if an assertion fails early, the client is destroyed in TearDown and flushes
// any remaining callbacks after this test body has returned.
struct CallbackState {
  explicit CallbackState(size_t expected) : responses(expected) {}

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Response> responses;
  std::vector<std::thread::id> threads;
  std::vector<bool> inLoopThread;
  size_t completed = 0;
};

class AsyncCallTest : public ::testing::Test {
protected:
  void SetUp() override {
    loop_ = thread_.startLoop();
    runOnLoop([this] {
      server_ = std::make_unique<RpcServer>(loop_, InetAddress(0, "127.0.0.1"));
      server_->registry()->registerMethod(
          "Echo", "Echo",
          [](std::string_view payload) { return std::string(payload); });
      server_->start();
      port_ = server_->listenAddress().toPort();
    });
    ASSERT_NE(port_, 0u) << "the server did not report its listening port";

    runOnLoop([this] {
      client_ = std::make_unique<RpcClient>(loop_, InetAddress(port_, "127.0.0.1"));
    });
    client_->connect();
  }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    destroyOnLoop(client_);
    destroyOnLoop(server_);
  }

  template <typename Fn> void runOnLoop(Fn &&fn) {
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    loop_->runInLoop([fn = std::forward<Fn>(fn), done]() mutable {
      fn();
      done->set_value();
    });
    ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not run the task in time";
  }

  // No assertions: this also runs from TearDown after a failed test.
  template <typename T> void destroyOnLoop(std::unique_ptr<T> &object) {
    if (!object) {
      return;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    loop_->runInLoop([&object, done] {
      object.reset();
      done->set_value();
    });
    finished.wait_for(kWaitTimeout);
  }

  std::thread::id loopThreadId() {
    std::thread::id id;
    runOnLoop([&id] { id = std::this_thread::get_id(); });
    return id;
  }

  bool waitForCompletions(const std::shared_ptr<CallbackState> &state,
                          size_t expected) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, kWaitTimeout, [&state, expected] {
      return state->completed == expected;
    });
  }

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  uint16_t port_ = 0;
  std::unique_ptr<RpcServer> server_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(AsyncCallTest, FiftyAsyncCallsAllCompleteWithTheirOwnResponse) {
  constexpr int kCalls = 50;
  auto state = std::make_shared<CallbackState>(kCalls);

  for (int i = 0; i < kCalls; ++i) {
    client_->AsyncCall(
        Request{"Echo", "Echo", std::to_string(i)},
        [state, i](Response response) {
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->responses[static_cast<size_t>(i)] = std::move(response);
            ++state->completed;
          }
          state->cv.notify_one();
        });
  }

  ASSERT_TRUE(waitForCompletions(state, kCalls))
      << "only " << state->completed << " of " << kCalls << " callbacks ran";
  for (int i = 0; i < kCalls; ++i) {
    const Response &response = state->responses[static_cast<size_t>(i)];
    EXPECT_EQ(response.status, StatusCode::kOk) << "call " << i;
    EXPECT_EQ(response.payload, std::to_string(i)) << "call " << i;
  }
}

TEST_F(AsyncCallTest, CallbacksRunOnTheLoopThread) {
  const std::thread::id loopId = loopThreadId();
  auto state = std::make_shared<CallbackState>(1);

  client_->AsyncCall(Request{"Echo", "Echo", "thread"},
                     [state, loop = loop_](Response response) {
                       {
                         std::lock_guard<std::mutex> lock(state->mutex);
                         state->responses[0] = std::move(response);
                         state->threads.push_back(std::this_thread::get_id());
                         state->inLoopThread.push_back(loop->isInLoopThread());
                         ++state->completed;
                       }
                       state->cv.notify_one();
                     });

  ASSERT_TRUE(waitForCompletions(state, 1));
  ASSERT_EQ(state->threads.size(), 1u);
  EXPECT_EQ(state->threads[0], loopId)
      << "the callback did not run on the loop thread";
  EXPECT_TRUE(state->inLoopThread[0]);
}

TEST_F(AsyncCallTest, ImmediateFailureCallbackAlsoRunsOnTheLoopThread) {
  const std::thread::id loopId = loopThreadId();
  auto state = std::make_shared<CallbackState>(1);

  // Never connected: the call fails, but the callback still belongs to the
  // loop. Constructing and destroying it on the loop mirrors the API contract.
  std::unique_ptr<RpcClient> offline;
  runOnLoop([this, &offline] {
    offline =
        std::make_unique<RpcClient>(loop_, InetAddress(port_, "127.0.0.1"));
  });

  offline->AsyncCall(Request{"Echo", "Echo", "x"},
                     [state, loop = loop_](Response response) {
                       {
                         std::lock_guard<std::mutex> lock(state->mutex);
                         state->responses[0] = std::move(response);
                         state->threads.push_back(std::this_thread::get_id());
                         state->inLoopThread.push_back(loop->isInLoopThread());
                         ++state->completed;
                       }
                       state->cv.notify_one();
                     });

  ASSERT_TRUE(waitForCompletions(state, 1));
  EXPECT_EQ(state->responses[0].status, StatusCode::kInternalError);
  ASSERT_EQ(state->threads.size(), 1u);
  EXPECT_EQ(state->threads[0], loopId)
      << "an immediate failure must not run the callback on the caller thread";
  EXPECT_TRUE(state->inLoopThread[0]);

  runOnLoop([&offline] { offline.reset(); });
}

TEST_F(AsyncCallTest, CallAsyncFuturesAreSafeToWaitAfterSending) {
  constexpr int kCalls = 30;

  // Send everything first: waiting must not be required to make progress.
  std::vector<std::future<Response>> futures;
  futures.reserve(kCalls);
  for (int i = 0; i < kCalls; ++i) {
    futures.push_back(
        client_->CallAsync(Request{"Echo", "Echo", std::to_string(i)}));
  }

  for (int i = 0; i < kCalls; ++i) {
    const Response response = futures[static_cast<size_t>(i)].get();
    EXPECT_EQ(response.status, StatusCode::kOk) << "call " << i;
    EXPECT_EQ(response.payload, std::to_string(i)) << "call " << i;
  }
}

TEST_F(AsyncCallTest, CallAsyncDoesNotBlockOnTheLoopThread) {
  // A future produced on the loop thread is only touched after the task
  // returns; if CallAsync() waited, this task could never finish.
  auto holder = std::make_shared<std::future<Response>>();
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  loop_->runInLoop([this, holder, done] {
    *holder = client_->CallAsync(Request{"Echo", "Echo", "loop"});
    done->set_value();
  });

  ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
      << "CallAsync blocked on the loop thread";
  const Response response = holder->get();
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "loop");
}

} // namespace
