#include <gtest/gtest.h>

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "rpc/rpc_client.h"
#include "rpc_fixture.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace reactornet::rpc::testing;

// Cross-thread Call() contract (#36): any business thread may share one
// RpcClient, while the client's own loop thread must never block in Call().
namespace {

class ConcurrentCallTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.started()) << fixture_.error();
    ASSERT_TRUE(fixture_.runOnLoop([this] {
      client_ = std::make_shared<RpcClient>(fixture_.loop(), fixture_.addr());
    }));
    client_->connect();
  }

  void TearDown() override {
    // The client is loop-thread owned, and every call has returned by now.
    fixture_.runOnLoop([this] { client_.reset(); });
  }

  RpcServerFixture fixture_;
  std::shared_ptr<RpcClient> client_;
};

TEST_F(ConcurrentCallTest, EightBusinessThreadsEachSendFiftyCalls) {
  constexpr int kThreads = 8;
  constexpr int kCallsPerThread = 50;

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, t, &failures] {
      for (int i = 0; i < kCallsPerThread; ++i) {
        const std::string payload =
            "t" + std::to_string(t) + "-" + std::to_string(i);
        const Response response =
            client_->Call(Request{"Echo", "Echo", payload});
        if (response.status != StatusCode::kOk ||
            response.payload != payload) {
          ++failures;
        }
      }
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }

  EXPECT_EQ(failures.load(), 0);
}

// The synchronous form cannot work on the loop thread: the response could
// only be delivered by the very thread that is blocked. The contract is
// enforced by an assertion, so the only honest test is a death test.
#if !defined(NDEBUG) && !defined(__SANITIZE_THREAD__)
TEST(ConcurrentCallDeathTest, SyncCallOnTheLoopThreadAsserts) {
  EXPECT_DEATH(
      {
        EventLoopThread thread;
        EventLoop *loop = thread.startLoop();
        // Nothing ever connects: the assertion must fire before any I/O.
        auto client = std::make_shared<RpcClient>(
            loop, InetAddress(1, "127.0.0.1"));
        loop->runInLoop([client] {
          (void)client->Call(Request{"Echo", "Echo", "deadlock"});
        });
        std::this_thread::sleep_for(std::chrono::seconds(1));
      },
      "loop thread");
}
#endif

} // namespace
