#include <gtest/gtest.h>

#include "rpc/codec.h"
#include "rpc_fixture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace reactornet::rpc::testing;
using namespace std::chrono_literals;

// End-to-end tests for RpcServer (#31). The client is a raw socket on purpose:
// the assertions are about bytes on the wire, not about our own client stack.
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

std::string requestFrame(uint32_t requestId, const std::string &service,
                         const std::string &method,
                         const std::string &payload) {
  const std::string body = envelope(service, method, payload);

  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kRequest;
  header.flags = 0;
  header.status = StatusCode::kOk;
  header.requestId = requestId;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  return std::string(encoded.data(), encoded.size()) + body;
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
        *error = std::string("send: ") + std::strerror(errno);
        return false;
      }
    }
    return true;
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

} // namespace

class RpcServerTest : public ::testing::Test {
protected:
  // The fixture starts the echo server on port 0 and tears it down on the
  // loop thread; a fatal failure here skips the test body.
  void SetUp() override {
    ASSERT_TRUE(fixture_.started()) << fixture_.error();
  }

  RpcServerFixture fixture_;
};

TEST_F(RpcServerTest, HandWrittenRequestProducesExactResponseBytes) {
  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(fixture_.port(), &error)) << error;

  // Header + body, all bytes spelled out (RFC-0001 §4). request_id = 42.
  // Arrays, not const char*: the header contains NUL bytes, and std::string
  // would stop at the first one.
  constexpr char kRequestBytes[] = "\x52\x4e" "\x01\x01" "\x00\x00" "\x00\x00" "\x2a\x00\x00\x00" "\x11\x00\x00\x00" "\x04\x00" "Echo" "\x04\x00" "Echo" "hello";
  const std::string request(kRequestBytes, sizeof(kRequestBytes) - 1);
  ASSERT_EQ(request.size(), 33u);

  ASSERT_TRUE(client.sendAll(request, &error)) << error;

  constexpr char kExpectedBytes[] = "\x52\x4e" "\x01\x02" "\x00\x00" "\x00\x00" "\x2a\x00\x00\x00" "\x05\x00\x00\x00" "hello";
  const std::string expected(kExpectedBytes, sizeof(kExpectedBytes) - 1);
  ASSERT_EQ(expected.size(), 21u);

  std::string received;
  ASSERT_TRUE(client.recvExactly(expected.size(), &received, &error))
      << error;
  EXPECT_EQ(received, expected);
}

TEST_F(RpcServerTest, TwoRequestsInOneSegmentYieldTwoResponses) {
  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(fixture_.port(), &error)) << error;

  const std::string first = requestFrame(1, "Echo", "Echo", "first");
  const std::string second = requestFrame(2, "Echo", "Echo", "second");
  ASSERT_TRUE(client.sendAll(first + second, &error)) << error;

  MessageHeader header{};
  std::string body;
  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.type, MessageType::kResponse);
  EXPECT_EQ(header.requestId, 1u);
  EXPECT_EQ(header.status, StatusCode::kOk);
  EXPECT_EQ(body, "first");

  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.requestId, 2u);
  EXPECT_EQ(header.status, StatusCode::kOk);
  EXPECT_EQ(body, "second");
}

TEST_F(RpcServerTest, RequestSplitAcrossTwoSegmentsYieldsOneResponse) {
  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(fixture_.port(), &error)) << error;

  const std::string frame = requestFrame(3, "Echo", "Echo", "split body");
  const size_t split = 10; // inside the header
  ASSERT_TRUE(client.sendAll(frame.substr(0, split), &error)) << error;
  std::this_thread::sleep_for(50ms); // let the server see an incomplete frame
  ASSERT_TRUE(client.sendAll(frame.substr(split), &error)) << error;

  MessageHeader header{};
  std::string body;
  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.requestId, 3u);
  EXPECT_EQ(header.status, StatusCode::kOk);
  EXPECT_EQ(body, "split body");
}

TEST_F(RpcServerTest, UnknownMethodAndServiceKeepTheConnectionOpen) {
  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(fixture_.port(), &error)) << error;

  MessageHeader header{};
  std::string body;

  ASSERT_TRUE(client.sendAll(requestFrame(10, "Echo", "Nope", "x"), &error))
      << error;
  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.requestId, 10u);
  EXPECT_EQ(header.status, StatusCode::kUnknownMethod);
  EXPECT_EQ(body, "");

  ASSERT_TRUE(client.sendAll(requestFrame(11, "NoSvc", "Echo", "x"), &error))
      << error;
  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.requestId, 11u);
  EXPECT_EQ(header.status, StatusCode::kUnknownService);
  EXPECT_EQ(body, "");

  // The same connection still serves calls after two misses.
  ASSERT_TRUE(
      client.sendAll(requestFrame(12, "Echo", "Echo", "still alive"), &error))
      << error;
  ASSERT_TRUE(recvFrame(&client, &header, &body, &error)) << error;
  EXPECT_EQ(header.requestId, 12u);
  EXPECT_EQ(header.status, StatusCode::kOk);
  EXPECT_EQ(body, "still alive");
}

TEST_F(RpcServerTest, StartFreezesTheRegistry) {
  EXPECT_TRUE(fixture_.registry()->frozen());
  EXPECT_FALSE(fixture_.registry()->registerMethod(
      "Late", "Method", [](std::string_view) { return std::string(); }));
}
