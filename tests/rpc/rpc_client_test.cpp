#include <gtest/gtest.h>

#include "Buffer.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include "rpc/rpc_client.h"
#include "rpc_fixture.h"

#include <memory>
#include <string>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace reactornet::rpc::testing;

// End-to-end tests for RpcClient (#32). The fixture hosts the echo server and
// the loop; the test thread is the only one that ever calls the blocking
// connect()/Call(). No test hard-codes a port: both servers bind port 0 and
// listenAddress() reports the choice.
namespace {

using TransportPtr = Transport::TransportPtr;

class RpcE2E : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.started()) << fixture_.error();
    port_ = fixture_.port();
  }

  void TearDown() override {
    // Loop-thread-owned objects must not be destroyed by the fixture's
    // destructor (which runs on the test thread).
    destroyOnLoop(client_);
    destroyOnLoop(closingServer_);
  }

  // Consumes the request and closes without answering: Call() must return an
  // error instead of waiting forever for a response that cannot come.
  void startClosingServer() {
    ASSERT_TRUE(fixture_.runOnLoop([this] {
      closingServer_ = std::make_unique<TcpServer>(
          fixture_.loop(), InetAddress(0, "127.0.0.1"), "ClosingServer");
      closingServer_->setMessageCallback(
          [](const TransportPtr &conn, Buffer *buf, Timestamp) {
            buf->retrieveAll();
            conn->forceClose();
          });
      closingServer_->start();
      port_ = closingServer_->listenAddress().toPort();
    }));
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  void connectClient() {
    ASSERT_TRUE(fixture_.runOnLoop([this] {
      client_ = std::make_unique<RpcClient>(
          fixture_.loop(), InetAddress(port_, "127.0.0.1"));
    }));
    client_->connect();
  }

  // No assertions: this also runs from TearDown after a failed test.
  template <typename T> void destroyOnLoop(std::unique_ptr<T> &object) {
    if (!object) {
      return;
    }
    fixture_.runOnLoop([&object] { object.reset(); });
  }

  RpcServerFixture fixture_;
  uint16_t port_ = 0;
  std::unique_ptr<TcpServer> closingServer_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(RpcE2E, EchoRoundTrip) {
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response response =
      client_->Call(Request{"Echo", "Echo", "hello rpc"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "hello rpc");
}

TEST_F(RpcE2E, SequentialCallsAfterThePreviousReturned) {
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response first = client_->Call(Request{"Echo", "Echo", "first"});
  ASSERT_EQ(first.status, StatusCode::kOk);
  EXPECT_EQ(first.payload, "first");

  const Response second = client_->Call(Request{"Echo", "Echo", "second"});
  ASSERT_EQ(second.status, StatusCode::kOk);
  EXPECT_EQ(second.payload, "second");
}

TEST_F(RpcE2E, UnknownMethodReturnsStatusInsteadOfThrowing) {
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
  EXPECT_EQ(response.status, StatusCode::kInternalError);
  EXPECT_EQ(response.payload, "");
}

} // namespace
