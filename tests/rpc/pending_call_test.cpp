#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include "rpc/rpc_client.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace std::chrono_literals;

// Tests for the pending-call table and response correlation (#34). The server
// below is a raw TcpServer on purpose: it lets the test choose the response
// order (reverse arrival) and borrow a wrong request id, neither of which
// RpcServer exposes.
namespace {

using TransportPtr = Transport::TransportPtr;

constexpr auto kWaitTimeout = 5s;

class TestServer {
public:
  enum class Mode {
    kReverseOrder, // answer the whole batch in reverse arrival order
    kBadRequestId, // answer with requestId + 1
  };

  TestServer(EventLoop *loop, int expectedCalls, Mode mode)
      : loop_(loop), expectedCalls_(expectedCalls), mode_(mode),
        server_(loop, InetAddress(0, "127.0.0.1"), "PendingCallTestServer"),
        down_(std::make_shared<std::promise<void>>()),
        downFuture_(down_->get_future().share()) {
    server_.setConnectionCallback([this](const TransportPtr &conn) {
      if (!conn->connected() && !downSignaled_) {
        downSignaled_ = true;
        down_->set_value();
      }
    });
    server_.setMessageCallback(
        [this](const TransportPtr &conn, Buffer *buf, Timestamp) {
          onMessage(conn, buf);
        });
    server_.start();
    port_ = server_.listenAddress().toPort();
  }

  uint16_t port() const { return port_; }

  // Loop thread only.
  std::vector<int> arrivalOrder() const { return arrivalOrder_; }
  std::vector<int> responseOrder() const { return responseOrder_; }

  std::shared_future<void> downFuture() const { return downFuture_; }

private:
  void onMessage(const TransportPtr &conn, Buffer *buf) {
    loop_->assertInLoopThread();

    for (;;) {
      MessageHeader header{};
      std::string body;
      const ParseResult result = frameReader_.parse(buf, &header, &body);
      if (result == ParseResult::kNeedMore) {
        return;
      }
      if (result != ParseResult::kOk) {
        conn->forceClose();
        return;
      }

      Request request;
      if (!codec_.decodeRequest(body, &request)) {
        conn->forceClose();
        return;
      }

      if (mode_ == Mode::kBadRequestId) {
        sendResponse(conn, header.requestId + 1, request.payload);
        continue;
      }

      arrivalOrder_.push_back(std::stoi(request.payload));
      pendingRequests_.emplace_back(header.requestId, request.payload);
      if (static_cast<int>(pendingRequests_.size()) == expectedCalls_) {
        flushInReverseOrder(conn);
      }
    }
  }

  // Wait until every request has arrived, then answer in reverse arrival
  // order. Collecting the batch first makes the reversal independent of how
  // spread out the senders happened to be; TCP keeps the writes in this order.
  void flushInReverseOrder(const TransportPtr &conn) {
    for (auto it = pendingRequests_.rbegin(); it != pendingRequests_.rend();
         ++it) {
      responseOrder_.push_back(std::stoi(it->second));
      sendResponse(conn, it->first, it->second);
    }
    pendingRequests_.clear();
  }

  void sendResponse(const TransportPtr &conn, uint32_t requestId,
                    const std::string &payload) {
    MessageHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.type = MessageType::kResponse;
    header.flags = 0;
    header.status = StatusCode::kOk;
    header.requestId = requestId;
    header.bodyLen = static_cast<uint32_t>(payload.size());

    std::array<char, kHeaderSize> encoded{};
    encodeHeader(header, encoded.data());

    std::string frame(encoded.data(), encoded.size());
    frame += payload;
    conn->send(frame);
  }

  EventLoop *const loop_;
  const int expectedCalls_;
  const Mode mode_;
  TcpServer server_;
  RawCodec codec_;
  FrameReader frameReader_;
  uint16_t port_ = 0;
  std::vector<std::pair<uint32_t, std::string>> pendingRequests_; // loop only
  std::vector<int> arrivalOrder_;  // loop thread only
  std::vector<int> responseOrder_; // loop thread only
  std::shared_ptr<std::promise<void>> down_;
  std::shared_future<void> downFuture_;
  bool downSignaled_ = false;
};

class PendingCallTest : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    destroyOnLoop(client_);
    destroyOnLoop(server_);
  }

  void startServer(int expectedCalls,
                   TestServer::Mode mode = TestServer::Mode::kReverseOrder) {
    runOnLoop([this, expectedCalls, mode] {
      server_ = std::make_unique<TestServer>(loop_, expectedCalls, mode);
      port_ = server_->port();
    });
    ASSERT_NE(port_, 0u) << "the server did not report its listening port";
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

  std::vector<int> arrivalOrder() {
    std::vector<int> order;
    runOnLoop([this, &order] { order = server_->arrivalOrder(); });
    return order;
  }

  std::vector<int> responseOrder() {
    std::vector<int> order;
    runOnLoop([this, &order] { order = server_->responseOrder(); });
    return order;
  }

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  uint16_t port_ = 0;
  std::unique_ptr<TestServer> server_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(PendingCallTest, OutOfOrderResponsesReachTheRightCaller) {
  constexpr int kCalls = 20;
  ASSERT_NO_FATAL_FAILURE(startServer(kCalls));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  std::vector<Response> responses(kCalls);
  std::vector<std::thread> callers;
  callers.reserve(kCalls);
  for (int i = 0; i < kCalls; ++i) {
    callers.emplace_back([this, i, &responses] {
      responses[static_cast<size_t>(i)] =
          client_->Call(Request{"Echo", "Echo", std::to_string(i)});
    });
  }
  for (auto &caller : callers) {
    caller.join();
  }

  for (int i = 0; i < kCalls; ++i) {
    EXPECT_EQ(responses[static_cast<size_t>(i)].status, StatusCode::kOk)
        << "call " << i;
    EXPECT_EQ(responses[static_cast<size_t>(i)].payload, std::to_string(i))
        << "call " << i;
  }

  // Prove the responses really were reordered: the wire order must be the
  // exact reverse of the order in which the server received the requests.
  const std::vector<int> arrival = arrivalOrder();
  const std::vector<int> order = responseOrder();
  ASSERT_EQ(arrival.size(), static_cast<size_t>(kCalls));
  ASSERT_EQ(order.size(), static_cast<size_t>(kCalls));
  for (int i = 0; i < kCalls; ++i) {
    EXPECT_EQ(order[static_cast<size_t>(i)],
              arrival[static_cast<size_t>(kCalls - 1 - i)])
        << "response " << i << " left the server out of order";
  }
  EXPECT_NE(order, arrival) << "the server did not reorder the responses";
}

TEST_F(PendingCallTest, OneHundredConcurrentCallsAllMatch) {
  constexpr int kCalls = 100;
  ASSERT_NO_FATAL_FAILURE(startServer(kCalls));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  std::vector<Response> responses(kCalls);
  std::vector<std::thread> callers;
  callers.reserve(kCalls);
  for (int i = 0; i < kCalls; ++i) {
    callers.emplace_back([this, i, &responses] {
      responses[static_cast<size_t>(i)] =
          client_->Call(Request{"Echo", "Echo", std::to_string(i)});
    });
  }
  for (auto &caller : callers) {
    caller.join();
  }

  for (int i = 0; i < kCalls; ++i) {
    EXPECT_EQ(responses[static_cast<size_t>(i)].status, StatusCode::kOk)
        << "call " << i;
    EXPECT_EQ(responses[static_cast<size_t>(i)].payload, std::to_string(i))
        << "call " << i;
  }
}

TEST_F(PendingCallTest, UnknownResponseIdClosesTheConnection) {
  ASSERT_NO_FATAL_FAILURE(startServer(1, TestServer::Mode::kBadRequestId));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response response = client_->Call(Request{"Echo", "Echo", "0"});
  EXPECT_EQ(response.status, StatusCode::kInternalError);

  // The client must tear the connection down, not merely ignore the frame.
  const std::shared_future<void> down = server_->downFuture();
  EXPECT_EQ(down.wait_for(kWaitTimeout), std::future_status::ready)
      << "the client did not close the connection after a bad response id";
}

} // namespace
