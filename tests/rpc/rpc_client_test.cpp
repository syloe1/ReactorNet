#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include "rpc/rpc_client.h"
#include "rpc/rpc_server.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace std::chrono_literals;

// End-to-end tests for RpcClient (#32). One EventLoopThread hosts both ends,
// so every callback runs on one thread while the test thread is the only one
// that ever calls the blocking connect()/Call(). No test hard-codes a port:
// the server is created with port 0 and listenAddress() reports the choice.
namespace {

using TransportPtr = Transport::TransportPtr;

constexpr auto kWaitTimeout = 5s;

class RpcE2E : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    // Loop-thread-owned objects must not be destroyed by the fixture's
    // destructor (which runs on the test thread).
    destroyOnLoop(client_);
    destroyOnLoop(server_);
    destroyOnLoop(rawServer_);
  }

  void startEchoServer() {
    runOnLoop([this] {
      server_ = std::make_unique<RpcServer>(loop_, InetAddress(0, "127.0.0.1"));
      server_->registry()->registerMethod(
          "Echo", "Echo",
          [](std::string_view payload) { return std::string(payload); });
      server_->start();
      port_ = server_->listenAddress().toPort();
    });
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  // Consumes the request and closes without answering: Call() must return an
  // error instead of waiting forever for a response that cannot come.
  void startClosingServer() {
    runOnLoop([this] {
      rawServer_ = std::make_unique<TcpServer>(
          loop_, InetAddress(0, "127.0.0.1"), "ClosingServer");
      rawServer_->setMessageCallback(
          [](const TransportPtr &conn, Buffer *buf, Timestamp) {
            buf->retrieveAll();
            conn->forceClose();
          });
      rawServer_->start();
      port_ = rawServer_->listenAddress().toPort();
    });
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  void connectClient() {
    runOnLoop([this] {
      client_ = std::make_unique<RpcClient>(
          loop_, InetAddress(port_, "127.0.0.1"));
    });
    client_->connect();
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

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  uint16_t port_ = 0;
  std::unique_ptr<RpcServer> server_;
  std::unique_ptr<TcpServer> rawServer_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(RpcE2E, EchoRoundTrip) {
  ASSERT_NO_FATAL_FAILURE(startEchoServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response response =
      client_->Call(Request{"Echo", "Echo", "hello rpc"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "hello rpc");
}

TEST_F(RpcE2E, SequentialCallsAfterThePreviousReturned) {
  ASSERT_NO_FATAL_FAILURE(startEchoServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response first = client_->Call(Request{"Echo", "Echo", "first"});
  ASSERT_EQ(first.status, StatusCode::kOk);
  EXPECT_EQ(first.payload, "first");

  const Response second = client_->Call(Request{"Echo", "Echo", "second"});
  ASSERT_EQ(second.status, StatusCode::kOk);
  EXPECT_EQ(second.payload, "second");
}

TEST_F(RpcE2E, UnknownMethodReturnsStatusInsteadOfThrowing) {
  ASSERT_NO_FATAL_FAILURE(startEchoServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response response = client_->Call(Request{"Echo", "Nope", "x"});
  EXPECT_EQ(response.status, StatusCode::kUnknownMethod);
  EXPECT_EQ(response.payload, "");
}

TEST_F(RpcE2E, PeerCloseWhileCallPendingReturnsError) {
  ASSERT_NO_FATAL_FAILURE(startClosingServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  // Without the close callback resolving the promise, this would block until
  // ctest's timeout instead of returning.
  const Response response =
      client_->Call(Request{"Echo", "Echo", "no answer"});
  EXPECT_EQ(response.status, StatusCode::kConnectionClosed);
  EXPECT_EQ(response.payload, "");
}

} // namespace
