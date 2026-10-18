#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include "rpc/rpc_client.h"
#include "rpc/rpc_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace std::chrono_literals;

// Error model tests (#39). The contract: every failure is a Response with a
// status that says whose fault it was, nothing throws across the API, and a
// misbehaving handler cannot take down the server, the connection, or another
// client.
namespace {

using TransportPtr = Transport::TransportPtr;

constexpr auto kWaitTimeout = 5s;

// A port that was just bound and released: connecting to it is refused
// promptly and deterministically on the test host.
uint16_t closedPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return 1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
    ::close(fd);
    return 1;
  }
  const uint16_t port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

class ErrorModelTest : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    destroyOnLoop(client_);
    destroyOnLoop(rawServer_);
    destroyOnLoop(server_);
  }

  // A server whose methods exercise both the failure and the success paths.
  void startRpcServer() {
    runOnLoop([this] {
      server_ = std::make_unique<RpcServer>(loop_, InetAddress(0, "127.0.0.1"));
      server_->registry()->registerMethod(
          "Echo", "Echo",
          [](std::string_view payload) { return std::string(payload); });
      server_->registry()->registerMethod(
          "Throwing", "Std",
          [](std::string_view) -> std::string {
            throw std::runtime_error("handler failed");
          });
      // A handler may throw anything, not just std::exception subclasses.
      server_->registry()->registerMethod(
          "Throwing", "Unknown", [](std::string_view) -> std::string {
            throw 42;
          });
      server_->start();
      port_ = server_->listenAddress().toPort();
    });
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  // Consumes requests and never answers, leaving calls pending until the
  // connection itself goes away.
  void startSilentServer() {
    runOnLoop([this] {
      rawServer_ = std::make_unique<TcpServer>(
          loop_, InetAddress(0, "127.0.0.1"), "SilentServer");
      rawServer_->setMessageCallback(
          [](const TransportPtr &, Buffer *buf, Timestamp) {
            buf->retrieveAll();
          });
      rawServer_->start();
      port_ = rawServer_->listenAddress().toPort();
    });
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  // Consumes the request and closes without answering.
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

  // Constructs the client on the loop (its transport is loop-thread owned) and
  // blocks the test thread until the handshake completes.
  void connectClient() {
    runOnLoop([this] {
      client_ = std::make_unique<RpcClient>(
          loop_, InetAddress(port_, "127.0.0.1"));
    });
    client_->connect();
  }

  std::unique_ptr<RpcClient> makeConnectedClient() {
    std::unique_ptr<RpcClient> client;
    runOnLoop([this, &client] {
      client =
          std::make_unique<RpcClient>(loop_, InetAddress(port_, "127.0.0.1"));
    });
    client->connect();
    return client;
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

TEST_F(ErrorModelTest, HandlerExceptionBecomesInternalError) {
  ASSERT_NO_FATAL_FAILURE(startRpcServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  EXPECT_EQ(client_->Call(Request{"Throwing", "Std", ""}).status,
            StatusCode::kInternalError);
  EXPECT_EQ(client_->Call(Request{"Throwing", "Unknown", ""}).status,
            StatusCode::kInternalError);
}

TEST_F(ErrorModelTest, HandlerExceptionLeavesTheConnectionUsable) {
  ASSERT_NO_FATAL_FAILURE(startRpcServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  // Two failures, then ordinary traffic on the very same connection. A caught
  // exception must not poison the stream: no response is lost and no frame is
  // half-written.
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(client_->Call(Request{"Throwing", "Std", ""}).status,
              StatusCode::kInternalError)
        << "iteration " << i;

    const std::string payload = "still-alive-" + std::to_string(i);
    const Response response = client_->Call(Request{"Echo", "Echo", payload});
    ASSERT_EQ(response.status, StatusCode::kOk) << "iteration " << i;
    ASSERT_EQ(response.payload, payload) << "iteration " << i;
  }
}

TEST_F(ErrorModelTest, HandlerExceptionOnOneClientDoesNotAffectAnother) {
  ASSERT_NO_FATAL_FAILURE(startRpcServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());
  std::unique_ptr<RpcClient> other = makeConnectedClient();

  EXPECT_EQ(client_->Call(Request{"Throwing", "Std", ""}).status,
            StatusCode::kInternalError);

  // A separate connection was never involved and keeps working.
  const Response response = other->Call(Request{"Echo", "Echo", "other"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "other");

  destroyOnLoop(other);
}

TEST_F(ErrorModelTest, UnknownServiceAndMethodPassThrough) {
  ASSERT_NO_FATAL_FAILURE(startRpcServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  EXPECT_EQ(client_->Call(Request{"NoSuchService", "Echo", "x"}).status,
            StatusCode::kUnknownService);
  EXPECT_EQ(client_->Call(Request{"Echo", "NoSuchMethod", "x"}).status,
            StatusCode::kUnknownMethod);

  // Application-level misses do not close the connection.
  const Response response = client_->Call(Request{"Echo", "Echo", "ok"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "ok");
}

TEST_F(ErrorModelTest, InvalidRequestIsRejectedBeforeSending) {
  ASSERT_NO_FATAL_FAILURE(startRpcServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  EXPECT_EQ(client_->Call(Request{"", "Echo", "x"}).status,
            StatusCode::kBadRequest);
  EXPECT_EQ(client_->Call(Request{"Echo", "", "x"}).status,
            StatusCode::kBadRequest);
  // Longer than the uint16 length prefix can represent.
  EXPECT_EQ(client_->Call(Request{std::string(70000, 'a'), "Echo", "x"}).status,
            StatusCode::kBadRequest);

  // Rejected locally: the connection was never touched and is still usable.
  const Response response = client_->Call(Request{"Echo", "Echo", "ok"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "ok");
}

TEST_F(ErrorModelTest, PeerCloseMidCallReturnsConnectionClosedQuickly) {
  ASSERT_NO_FATAL_FAILURE(startClosingServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const auto start = std::chrono::steady_clock::now();
  const Response response = client_->Call(Request{"Echo", "Echo", "x"});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(response.status, StatusCode::kConnectionClosed);
  EXPECT_EQ(response.payload, "");
  // The close resolves the call; there is no timer involved, so this must be
  // well below any deadline.
  EXPECT_LT(elapsed, 2s);
}

TEST_F(ErrorModelTest, FailedConnectIsReportedByTheNextCall) {
  const uint16_t port = closedPort();
  std::unique_ptr<RpcClient> offline;
  runOnLoop([this, port, &offline] {
    offline = std::make_unique<RpcClient>(
        loop_, InetAddress(port, "127.0.0.1"));
  });

  // connect() does not throw and does not hang: the failure surfaces on the
  // next call as a transport status.
  offline->connect();

  const auto start = std::chrono::steady_clock::now();
  const Response response = offline->Call(Request{"Echo", "Echo", "x"});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(response.status, StatusCode::kConnectionClosed);
  EXPECT_LT(elapsed, 2s);

  destroyOnLoop(offline);
}

TEST_F(ErrorModelTest, DisconnectFailsOutstandingCallsWithConnectionClosed) {
  ASSERT_NO_FATAL_FAILURE(startSilentServer());
  ASSERT_NO_FATAL_FAILURE(connectClient());

  auto promise = std::make_shared<std::promise<Response>>();
  auto future = promise->get_future();
  client_->AsyncCall(Request{"Echo", "Echo", "pending"},
                     [promise](Response response) {
                       promise->set_value(std::move(response));
                     });

  // The server never answers, so this call can only end because the
  // connection went away.
  client_->disconnect();

  ASSERT_EQ(future.wait_for(kWaitTimeout), std::future_status::ready)
      << "disconnect() did not resolve the outstanding call";
  const Response response = future.get();
  EXPECT_EQ(response.status, StatusCode::kConnectionClosed);
  EXPECT_EQ(response.payload, "");
}

TEST_F(ErrorModelTest, StatusToStringNamesEveryDefinedStatus) {
  EXPECT_STREQ(statusToString(StatusCode::kOk), "OK");
  EXPECT_STREQ(statusToString(StatusCode::kUnsupportedVersion),
               "UNSUPPORTED_VERSION");
  EXPECT_STREQ(statusToString(StatusCode::kUnsupportedMsgType),
               "UNSUPPORTED_MSG_TYPE");
  EXPECT_STREQ(statusToString(StatusCode::kUnsupportedFlag),
               "UNSUPPORTED_FLAG");
  EXPECT_STREQ(statusToString(StatusCode::kBadRequestId), "BAD_REQUEST_ID");
  EXPECT_STREQ(statusToString(StatusCode::kBodyTooLarge), "BODY_TOO_LARGE");
  EXPECT_STREQ(statusToString(StatusCode::kMalformedBody), "MALFORMED_BODY");
  EXPECT_STREQ(statusToString(StatusCode::kUnknownService),
               "UNKNOWN_SERVICE");
  EXPECT_STREQ(statusToString(StatusCode::kUnknownMethod), "UNKNOWN_METHOD");
  EXPECT_STREQ(statusToString(StatusCode::kInternalError), "INTERNAL_ERROR");
  EXPECT_STREQ(statusToString(StatusCode::kTimeout), "TIMEOUT");
  EXPECT_STREQ(statusToString(StatusCode::kConnectionClosed),
               "CONNECTION_CLOSED");
  EXPECT_STREQ(statusToString(StatusCode::kBadRequest), "BAD_REQUEST");

  // Reserved / application-defined values are not names this version knows.
  EXPECT_STREQ(statusToString(static_cast<StatusCode>(0x7F)), "UNKNOWN_STATUS");
  EXPECT_STREQ(statusToString(static_cast<StatusCode>(0xFF)), "UNKNOWN_STATUS");
}

} // namespace
