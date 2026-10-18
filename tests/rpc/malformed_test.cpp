#include <gtest/gtest.h>

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "rpc/rpc_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace std::chrono_literals;

// Hardening tests for malformed and oversized frames (#40). The client is a raw
// socket: these tests are about bytes on the wire, not about our own client.
namespace {

constexpr auto kIoTimeout = 5s;

std::string u16le(uint16_t value) {
  return std::string{static_cast<char>(value & 0xFFu),
                     static_cast<char>((value >> 8) & 0xFFu)};
}

std::string envelope(const std::string &service, const std::string &method,
                     const std::string &payload) {
  return u16le(static_cast<uint16_t>(service.size())) + service +
         u16le(static_cast<uint16_t>(method.size())) + method + payload;
}

std::string frame(uint8_t version, MessageType type, uint8_t flags,
                  uint32_t requestId, const std::string &body) {
  MessageHeader header{};
  header.magic = kMagic;
  header.version = version;
  header.type = type;
  header.flags = flags;
  header.status = StatusCode::kOk;
  header.requestId = requestId;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  return std::string(encoded.data(), encoded.size()) + body;
}

std::string requestFrame(uint32_t requestId, const std::string &service,
                         const std::string &method,
                         const std::string &payload) {
  return frame(kVersion, MessageType::kRequest, 0, requestId,
               envelope(service, method, payload));
}

class ClientSocket {
public:
  ClientSocket() = default;
  ClientSocket(const ClientSocket &) = delete;
  ClientSocket &operator=(const ClientSocket &) = delete;
  ~ClientSocket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool connectTo(uint16_t port, std::string *error) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      *error = std::string("socket: ") + std::strerror(errno);
      return false;
    }

    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(kIoTimeout.count());
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
      *error = "inet_pton failed";
      return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      *error = std::string("connect: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  bool sendAll(const std::string &data, std::string *error) {
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent,
                               MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        // The server may close while we are still sending (that is one of the
        // behaviours under test), so callers that expect it use
        // sendBestEffort().
        *error = std::string("send: ") + std::strerror(errno);
        return false;
      }
    }
    return true;
  }

  void sendBestEffort(const std::string &data) {
    std::string ignored;
    (void)sendAll(data, &ignored);
  }

  bool recvExactly(size_t len, std::string *out, std::string *error) {
    out->clear();
    out->reserve(len);
    while (out->size() < len) {
      char buf[4096];
      const size_t want = std::min(sizeof(buf), len - out->size());
      const ssize_t n = ::recv(fd_, buf, want, 0);
      if (n > 0) {
        out->append(buf, static_cast<size_t>(n));
      } else if (n == 0) {
        *error = "peer closed the connection";
        return false;
      } else if (errno == EINTR) {
        continue;
      } else {
        *error = std::string("recv: ") + std::strerror(errno);
        return false;
      }
    }
    return true;
  }

  // True when the peer closed the connection without sending anything more.
  bool expectEof(std::string *error) {
    char byte = 0;
    for (;;) {
      const ssize_t n = ::recv(fd_, &byte, 1, 0);
      if (n == 0) {
        return true;
      }
      if (n > 0) {
        *error = "expected EOF, received a byte";
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      *error = std::string("recv: ") + std::strerror(errno);
      return false;
    }
  }

private:
  int fd_ = -1;
};

bool recvFrame(ClientSocket *client, MessageHeader *header, std::string *body,
               std::string *error) {
  std::string headerBytes;
  if (!client->recvExactly(kHeaderSize, &headerBytes, error)) {
    return false;
  }
  if (!decodeHeader(headerBytes.data(), headerBytes.size(), header)) {
    *error = "response header did not decode";
    return false;
  }
  if (header->bodyLen > 1024u * 1024u) {
    *error = "response body length is implausible: " +
             std::to_string(header->bodyLen);
    return false;
  }
  return client->recvExactly(header->bodyLen, body, error);
}

// Resident set size in KiB, or 0 when /proc/self/status is unavailable.
size_t residentKb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::string_view rest(line);
      rest.remove_prefix(6);
      while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
      }
      size_t kb = 0;
      for (char c : rest) {
        if (c < '0' || c > '9') {
          break;
        }
        kb = kb * 10 + static_cast<size_t>(c - '0');
      }
      return kb;
    }
  }
  return 0;
}

class MalformedTest : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    if (server_) {
      auto done = std::make_shared<std::promise<void>>();
      auto finished = done->get_future();
      loop_->runInLoop([this, done] {
        server_.reset();
        done->set_value();
      });
      finished.wait_for(kIoTimeout);
    }
  }

  // maxInputBuffer is applied before start(), exactly where a deployment would
  // set it. 0 keeps the default derived from the frame-size limit.
  void startServer(size_t maxInputBuffer = 0) {
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    loop_->runInLoop([this, maxInputBuffer, done] {
      server_ =
          std::make_unique<RpcServer>(loop_, InetAddress(0, "127.0.0.1"));
      server_->registry()->registerMethod(
          "Echo", "Echo",
          [](std::string_view payload) { return std::string(payload); });
      if (maxInputBuffer != 0) {
        server_->setMaxInputBufferSize(maxInputBuffer);
      }
      server_->start();
      port_ = server_->listenAddress().toPort();
      done->set_value();
    });
    ASSERT_EQ(finished.wait_for(kIoTimeout), std::future_status::ready);
    ASSERT_NE(port_, 0u) << "listenAddress() did not report a bound port";
  }

  std::unique_ptr<ClientSocket> connectRaw() {
    auto client = std::make_unique<ClientSocket>();
    std::string error;
    EXPECT_TRUE(client->connectTo(port_, &error)) << error;
    return client;
  }

  // Sends one well-formed echo request and checks the round trip. Any failure
  // means the server did not survive a previous malformed peer.
  void expectEchoStillWorks(uint32_t requestId, const std::string &payload) {
    auto client = connectRaw();
    std::string error;
    ASSERT_TRUE(client->sendAll(requestFrame(requestId, "Echo", "Echo",
                                             payload),
                                &error))
        << error;

    MessageHeader header{};
    std::string body;
    ASSERT_TRUE(recvFrame(client.get(), &header, &body, &error)) << error;
    EXPECT_EQ(header.status, StatusCode::kOk);
    EXPECT_EQ(header.requestId, requestId);
    EXPECT_EQ(body, payload);
  }

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  std::unique_ptr<RpcServer> server_;
  uint16_t port_ = 0;
};

TEST_F(MalformedTest, OneGigabyteDeclaredBodyAllocatesNothing) {
  ASSERT_NO_FATAL_FAILURE(startServer());
  const size_t rssBeforeKb = residentKb();

  auto client = connectRaw();
  std::string error;

  // The classic length-prefix DoS: declare 1 GiB, send no body. The reader
  // must reject on the header alone.
  MessageHeader huge{};
  huge.magic = kMagic;
  huge.version = kVersion;
  huge.type = MessageType::kRequest;
  huge.flags = 0;
  huge.status = StatusCode::kOk;
  huge.requestId = 77;
  huge.bodyLen = 1024u * 1024u * 1024u;

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(huge, encoded.data());
  ASSERT_TRUE(client->sendAll(std::string(encoded.data(), encoded.size()),
                              &error))
      << error;

  // RFC-0001 §4.6: answer BODY_TOO_LARGE with the echoed request id, then
  // close. No body was ever read.
  MessageHeader response{};
  std::string body;
  ASSERT_TRUE(recvFrame(client.get(), &response, &body, &error)) << error;
  EXPECT_EQ(response.status, StatusCode::kBodyTooLarge);
  EXPECT_EQ(response.requestId, 77u);
  EXPECT_EQ(body, "");
  EXPECT_TRUE(client->expectEof(&error)) << error;

  const size_t rssAfterKb = residentKb();
  if (rssBeforeKb != 0 && rssAfterKb != 0) {
    // A real allocation of 1 GiB would dwarf this. The margin absorbs ordinary
    // allocator noise and sanitizer bookkeeping.
    EXPECT_LT(rssAfterKb - rssBeforeKb, 32u * 1024u)
        << "resident memory grew after a header-only request";
  }
}

TEST_F(MalformedTest, GarbageBytesCloseTheConnectionAndTheServerKeepsServing) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto garbageClient = connectRaw();
  std::string garbage;
  garbage.reserve(1024);
  for (int i = 0; i < 1024; ++i) {
    garbage.push_back(static_cast<char>((i * 131 + 17) & 0xFF));
  }
  garbageClient->sendBestEffort(garbage);

  std::string error;
  EXPECT_TRUE(garbageClient->expectEof(&error)) << error;

  // The whole point: one bad peer must not take the listener down.
  EXPECT_NO_FATAL_FAILURE(expectEchoStillWorks(1, "still here"));
}

TEST_F(MalformedTest, BadMagicClosesWithoutAResponse) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto client = connectRaw();
  // HTTP on an RPC port: a plausible real-world mistake. The peer is not
  // speaking RN, so replying with an RN frame would be meaningless.
  client->sendBestEffort("GET / HTTP/1.1\r\nHost: x\r\n\r\n");

  std::string error;
  EXPECT_TRUE(client->expectEof(&error))
      << "a bad magic must close the connection without a response: " << error;
}

TEST_F(MalformedTest, UnsupportedVersionIsAnsweredThenClosed) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto client = connectRaw();
  std::string error;
  // Version 2, otherwise a perfect echo request.
  ASSERT_TRUE(client->sendAll(
      frame(2, MessageType::kRequest, 0, 42,
            envelope("Echo", "Echo", "v2")),
      &error))
      << error;

  MessageHeader response{};
  std::string body;
  ASSERT_TRUE(recvFrame(client.get(), &response, &body, &error)) << error;
  EXPECT_EQ(response.status, StatusCode::kUnsupportedVersion);
  EXPECT_EQ(response.requestId, 42u);
  EXPECT_TRUE(client->expectEof(&error)) << error;
}

TEST_F(MalformedTest, UnsupportedMessageTypeIsAnsweredThenClosed) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto client = connectRaw();
  std::string error;
  const auto unknownType = static_cast<MessageType>(0x7F);
  ASSERT_TRUE(client->sendAll(
      frame(kVersion, unknownType, 0, 43, envelope("Echo", "Echo", "x")),
      &error))
      << error;

  MessageHeader response{};
  std::string body;
  ASSERT_TRUE(recvFrame(client.get(), &response, &body, &error)) << error;
  EXPECT_EQ(response.status, StatusCode::kUnsupportedMsgType);
  EXPECT_EQ(response.requestId, 43u);
  EXPECT_TRUE(client->expectEof(&error)) << error;
}

TEST_F(MalformedTest, UnsupportedFlagsAreAnsweredThenClosed) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto client = connectRaw();
  std::string error;
  ASSERT_TRUE(client->sendAll(
      frame(kVersion, MessageType::kRequest,
            static_cast<uint8_t>(kFlagCompressed), 44,
            envelope("Echo", "Echo", "x")),
      &error))
      << error;

  MessageHeader response{};
  std::string body;
  ASSERT_TRUE(recvFrame(client.get(), &response, &body, &error)) << error;
  EXPECT_EQ(response.status, StatusCode::kUnsupportedFlag);
  EXPECT_EQ(response.requestId, 44u);
  EXPECT_TRUE(client->expectEof(&error)) << error;
}

TEST_F(MalformedTest, MalformedEnvelopeIsAnsweredThenClosed) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  auto client = connectRaw();
  std::string error;
  // Envelope claims a 5-byte service name but contains only two bytes.
  const std::string body = u16le(5) + "ab";
  ASSERT_TRUE(client->sendAll(
      frame(kVersion, MessageType::kRequest, 0, 45, body), &error))
      << error;

  MessageHeader response{};
  std::string responseBody;
  ASSERT_TRUE(recvFrame(client.get(), &response, &responseBody, &error))
      << error;
  EXPECT_EQ(response.status, StatusCode::kMalformedBody);
  EXPECT_EQ(response.requestId, 45u);
  EXPECT_TRUE(client->expectEof(&error)) << error;
}

TEST_F(MalformedTest, PartialFrameAboveTheInputBufferLimitIsClosed) {
  // One KiB is far below the declared frame, and below the 4 MiB default, so
  // this isolates the second layer: the Buffer keeps the bytes parse() cannot
  // consume yet.
  ASSERT_NO_FATAL_FAILURE(startServer(1024));

  auto client = connectRaw();

  // A legal header (8 KiB body, below the frame limit) followed by more body
  // bytes than the configured input-buffer ceiling.
  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kRequest;
  header.flags = 0;
  header.status = StatusCode::kOk;
  header.requestId = 46;
  header.bodyLen = 8192;

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  std::string bytes(encoded.data(), encoded.size());
  bytes.append(2048, 'x');

  // The server may close mid-send, so a send error is not interesting here.
  client->sendBestEffort(bytes);

  std::string error;
  EXPECT_TRUE(client->expectEof(&error))
      << "the server did not close an incomplete frame above the input limit";

  // The listener is unaffected.
  EXPECT_NO_FATAL_FAILURE(expectEchoStillWorks(2, "alive"));
}

TEST_F(MalformedTest, OversizedPeerDoesNotBlockOtherConnections) {
  ASSERT_NO_FATAL_FAILURE(startServer());

  // An attacker declares 1 GiB and never sends it. The server must reject the
  // header immediately, so a normal client arriving at the same time is served
  // while the attacker's connection is still open.
  auto attacker = connectRaw();

  MessageHeader huge{};
  huge.magic = kMagic;
  huge.version = kVersion;
  huge.type = MessageType::kRequest;
  huge.flags = 0;
  huge.status = StatusCode::kOk;
  huge.requestId = 47;
  huge.bodyLen = 1024u * 1024u * 1024u;

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(huge, encoded.data());
  std::string error;
  ASSERT_TRUE(attacker->sendAll(std::string(encoded.data(), encoded.size()),
                                &error))
      << error;

  EXPECT_NO_FATAL_FAILURE(expectEchoStillWorks(3, "independent"));

  MessageHeader response{};
  std::string body;
  ASSERT_TRUE(recvFrame(attacker.get(), &response, &body, &error)) << error;
  EXPECT_EQ(response.status, StatusCode::kBodyTooLarge);
  EXPECT_EQ(response.requestId, 47u);
}

} // namespace
