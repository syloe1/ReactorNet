#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include "rpc/rpc_client.h"

#include <array>
#include <atomic>
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

// Deadline tests for RpcClient (#38). The server is a raw TcpServer so the
// test controls exactly when (or whether) a response is written; the client
// timer is the only clock involved.
namespace {

using TransportPtr = Transport::TransportPtr;

constexpr auto kWaitTimeout = 5s;

class DelayedServer {
public:
  enum class Mode {
    kNever,     // consume the request and never answer
    kDelay,     // answer every request after a fixed delay
    kEchoIfOk,  // answer only payloads starting with 'o'
  };

  DelayedServer(EventLoop *loop, Mode mode, std::chrono::milliseconds delay)
      : loop_(loop), mode_(mode), delay_(delay),
        server_(loop, InetAddress(0, "127.0.0.1"), "TimeoutTestServer") {
    server_.setMessageCallback(
        [this](const TransportPtr &conn, Buffer *buf, Timestamp) {
          onMessage(conn, buf);
        });
    server_.start();
    port_ = server_.listenAddress().toPort();
  }

  uint16_t port() const { return port_; }

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
      handle(conn, header.requestId, request.payload);
    }
  }

  void handle(const TransportPtr &conn, uint32_t requestId,
              const std::string &payload) {
    switch (mode_) {
    case Mode::kNever:
      return;
    case Mode::kEchoIfOk:
      if (!payload.empty() && payload.front() == 'o') {
        sendResponse(conn, requestId, payload);
      }
      return;
    case Mode::kDelay:
      // The shared_ptr keeps the connection alive until the timer fires.
      loop_->runAfter(
          std::chrono::duration<double>(delay_).count(),
          [this, conn, requestId, payload] {
            sendResponse(conn, requestId, payload);
          });
      return;
    }
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
  const Mode mode_;
  const std::chrono::milliseconds delay_;
  TcpServer server_;
  RawCodec codec_;
  FrameReader frameReader_;
  uint16_t port_ = 0;
};

class TimeoutTest : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    destroyOnLoop(client_);
    destroyOnLoop(server_);
  }

  void startServer(DelayedServer::Mode mode,
                   std::chrono::milliseconds delay = 0ms) {
    runOnLoop([this, mode, delay] {
      server_ = std::make_unique<DelayedServer>(loop_, mode, delay);
      port_ = server_->port();
    });
    ASSERT_NE(port_, 0u) << "the server did not report its listening port";
  }

  void connectClient() {
    runOnLoop([this] {
      client_ = std::make_unique<RpcClient>(loop_, InetAddress(port_, "127.0.0.1"));
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
  std::unique_ptr<DelayedServer> server_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(TimeoutTest, NeverRespondingServerTimesOut) {
  ASSERT_NO_FATAL_FAILURE(startServer(DelayedServer::Mode::kNever));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const auto start = std::chrono::steady_clock::now();
  const Response response =
      client_->Call(Request{"Echo", "Echo", "x"}, 0.1);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(response.status, StatusCode::kTimeout);
  EXPECT_EQ(response.payload, "");
  EXPECT_GE(elapsed, 50ms) << "the deadline fired too early";
  EXPECT_LT(elapsed, 2s) << "the deadline fired far too late";
}

TEST_F(TimeoutTest, NormalResponseWinsAgainstTheDeadline) {
  ASSERT_NO_FATAL_FAILURE(startServer(DelayedServer::Mode::kEchoIfOk));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  // A timeout long enough that the local echo always wins. The point is that
  // the response path completes the call at all when a timer is armed.
  for (int i = 0; i < 50; ++i) {
    const std::string payload = "ok-" + std::to_string(i);
    const Response response =
        client_->Call(Request{"Echo", "Echo", payload}, 0.5);
    ASSERT_EQ(response.status, StatusCode::kOk) << "call " << i;
    ASSERT_EQ(response.payload, payload) << "call " << i;
  }
}

TEST_F(TimeoutTest, LateResponseIsDiscardedWithoutClosingTheConnection) {
  ASSERT_NO_FATAL_FAILURE(
      startServer(DelayedServer::Mode::kDelay, 150ms));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  const Response timedOut =
      client_->Call(Request{"Echo", "Echo", "late"}, 0.05);
  ASSERT_EQ(timedOut.status, StatusCode::kTimeout);

  // This call is still pending when the first (late) response arrives. The
  // late id must be dropped, not treated as a protocol fault.
  const Response second = client_->Call(
      Request{"Echo", "Echo", "second"}, 2.0);
  EXPECT_EQ(second.status, StatusCode::kOk);
  EXPECT_EQ(second.payload, "second");

  // The connection survived both the timeout and the late response.
  const Response third = client_->Call(
      Request{"Echo", "Echo", "third"}, 2.0);
  EXPECT_EQ(third.status, StatusCode::kOk);
  EXPECT_EQ(third.payload, "third");
}

TEST_F(TimeoutTest, OneThousandTimeoutsLeaveNoPendingState) {
  ASSERT_NO_FATAL_FAILURE(startServer(DelayedServer::Mode::kNever));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  // ASan turns a leaked pending entry or timer into a report at teardown.
  for (int i = 0; i < 1000; ++i) {
    const Response response =
        client_->Call(Request{"Echo", "Echo", std::to_string(i)}, 0.001);
    ASSERT_EQ(response.status, StatusCode::kTimeout) << "call " << i;
  }
}

TEST_F(TimeoutTest, ConcurrentTimeoutsAndResponsesInterleave) {
  ASSERT_NO_FATAL_FAILURE(startServer(DelayedServer::Mode::kEchoIfOk));
  ASSERT_NO_FATAL_FAILURE(connectClient());

  constexpr int kThreads = 4;
  constexpr int kCallsPerThread = 50;
  std::atomic<int> failures{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, t, &failures] {
      for (int i = 0; i < kCallsPerThread; ++i) {
        if ((i % 2) == 0) {
          const std::string payload =
              "ok-" + std::to_string(t) + "-" + std::to_string(i);
          const Response response =
              client_->Call(Request{"Echo", "Echo", payload}, 2.0);
          if (response.status != StatusCode::kOk ||
              response.payload != payload) {
            ++failures;
          }
        } else {
          const Response response = client_->Call(
              Request{"Echo", "Echo", "no-answer"}, 0.05);
          if (response.status != StatusCode::kTimeout) {
            ++failures;
          }
        }
      }
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

} // namespace
