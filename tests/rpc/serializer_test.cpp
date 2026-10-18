#include <gtest/gtest.h>

#include "rpc/json_serializer.h"
#include "rpc/json_value.h"
#include "rpc/protocol.h"
#include "rpc/rpc_client.h"
#include "rpc/serializer.h"
#include "rpc_fixture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace reactornet;
using namespace reactornet::rpc;
using namespace reactornet::rpc::testing;

// #42: Codec owns the envelope, Serializer owns payload <-> object, and the
// frame header carries the serializer id so the two ends cannot silently
// disagree about the payload format.
namespace {

TEST(JsonValueTest, ObjectAndArrayAccess) {
  JsonValue object = JsonValue::makeObject();
  object["name"] = JsonValue("alice");
  object["tags"] = JsonValue::makeArray();
  object["tags"].asArray().push_back(JsonValue("a"));
  object["tags"].asArray().push_back(JsonValue("b"));

  ASSERT_TRUE(object.isObject());
  ASSERT_NE(object.find("name"), nullptr);
  EXPECT_EQ(object.find("name")->asString(), "alice");
  EXPECT_EQ(object.find("missing"), nullptr);
  ASSERT_EQ(object["tags"].asArray().size(), 2u);
  EXPECT_EQ(object["tags"][1].asString(), "b");

  // Insertion order is preserved; equality is structural and order-sensitive.
  EXPECT_EQ(object.asObject()[0].first, "name");
  JsonValue copy = object;
  EXPECT_EQ(copy, object);
  copy["name"] = JsonValue("bob");
  EXPECT_NE(copy, object);
}

TEST(JsonValueTest, IntegerAndDoubleStayDistinct) {
  const JsonValue integer = JsonValue::makeInteger(9007199254740993LL);
  const JsonValue number = JsonValue::makeNumber(1.5);
  EXPECT_EQ(integer.asInt(), 9007199254740993LL);
  EXPECT_TRUE(integer.isInteger());
  EXPECT_FALSE(number.isInteger());
  EXPECT_DOUBLE_EQ(number.asNumber(), 1.5);
  EXPECT_NE(integer, number);
}

TEST(RawSerializerTest, NameAndIdMatchTheWireConstant) {
  RawSerializer serializer;
  EXPECT_EQ(serializer.name(), "raw");
  EXPECT_EQ(serializer.id(), kSerializerRaw);
  EXPECT_EQ(serializer.id(), 0u);
}

TEST(RawSerializerTest, RoundTripsArbitraryBytes) {
  RawSerializer serializer;
  const std::string bytes("a\0b\xff\xfe", 5); // NUL and invalid UTF-8 on purpose

  std::string encoded;
  ASSERT_TRUE(serializer.encode(JsonValue(bytes), &encoded));
  EXPECT_EQ(encoded, bytes);

  JsonValue decoded;
  ASSERT_TRUE(serializer.decode(encoded, &decoded));
  ASSERT_TRUE(decoded.isString());
  EXPECT_EQ(decoded.asString(), bytes);
}

TEST(RawSerializerTest, RejectsNonStringValuesWithoutTouchingTheOutput) {
  RawSerializer serializer;
  std::string out = "sentinel";

  EXPECT_FALSE(serializer.encode(JsonValue::makeInteger(7), &out));
  EXPECT_EQ(out, "sentinel");
  EXPECT_FALSE(serializer.encode(JsonValue::makeObject(), &out));
  EXPECT_EQ(out, "sentinel");
}

// ---------------------------------------------------------------------------
// End-to-end: the serializer id is a header field, so it is negotiated per
// connection and checked on both sides.

class SerializerE2E : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(fixture_.started()) << fixture_.error(); }

  void TearDown() override {
    if (client_ != nullptr) {
      fixture_.runOnLoop([this] { client_.reset(); });
    }
  }

  void connectClient(uint8_t serializerId) {
    ASSERT_TRUE(fixture_.runOnLoop([this, serializerId] {
      client_ = std::make_unique<RpcClient>(fixture_.loop(),
                                            InetAddress(fixture_.port(),
                                                        "127.0.0.1"));
      client_->setSerializerId(serializerId);
    }));
    client_->connect();
  }

  RpcServerFixture fixture_;
  std::unique_ptr<RpcClient> client_;
};

TEST_F(SerializerE2E, DefaultIdIsRawOnBothSides) {
  EXPECT_TRUE(fixture_.runOnLoop(
      [this] { EXPECT_EQ(fixture_.server()->serializerId(), kSerializerRaw); }));
  connectClient(kSerializerRaw);
  EXPECT_EQ(client_->serializerId(), kSerializerRaw);

  const Response response = client_->Call(Request{"Echo", "Echo", "payload"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "payload");
  EXPECT_EQ(response.serializerId, kSerializerRaw);
}

TEST_F(SerializerE2E, MatchingNonDefaultIdRoundTrips) {
  ASSERT_TRUE(fixture_.runOnLoop(
      [this] { fixture_.server()->setSerializerId(kSerializerJson); }));
  connectClient(kSerializerJson);

  const Response response =
      client_->Call(Request{"Echo", "Echo", "still opaque to the codec"});
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "still opaque to the codec");
  EXPECT_EQ(response.serializerId, kSerializerJson);
}

TEST_F(SerializerE2E, JsonPayloadRoundTripsThroughTheEchoServer) {
  ASSERT_TRUE(fixture_.runOnLoop(
      [this] { fixture_.server()->setSerializerId(kSerializerJson); }));
  connectClient(kSerializerJson);

  JsonSerializer json;
  JsonValue request = JsonValue::makeObject();
  request["message"] = JsonValue("hello");
  request["count"] = JsonValue::makeInteger(3);
  request["tags"] = JsonValue::makeArray();
  request["tags"].asArray().push_back(JsonValue("a"));

  std::string payload;
  ASSERT_TRUE(json.encode(request, &payload));

  const Response response =
      client_->Call(Request{"Echo", "Echo", payload});
  ASSERT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.serializerId, kSerializerJson);

  JsonValue decoded;
  ASSERT_TRUE(json.decode(response.payload, &decoded));
  EXPECT_EQ(decoded, request);
}

TEST_F(SerializerE2E, ServerRejectsAnUnsupportedIdAndStaysUsable) {
  // Server keeps the default (raw); the client asks for JSON.
  connectClient(kSerializerJson);

  const Response first = client_->Call(Request{"Echo", "Echo", "x"});
  EXPECT_EQ(first.status, StatusCode::kUnsupportedSerializer);
  EXPECT_EQ(first.payload, "");
  EXPECT_EQ(first.serializerId, kSerializerJson) << "the id must be echoed";

  // The frame was well formed, so the rejection is per request, not fatal.
  const Response second = client_->Call(Request{"Echo", "Echo", "y"});
  EXPECT_EQ(second.status, StatusCode::kUnsupportedSerializer);
}

// ---------------------------------------------------------------------------
// A pre-#42 peer ignores the serializer byte (it was a reserved byte) and
// answers with zero. The client must turn that mismatch into a failure instead
// of handing the caller a payload in the wrong format.

class LegacyEchoServer {
public:
  LegacyEchoServer() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      return;
    }
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
        0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr *>(&addr), &len) !=
        0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return;
    }
    port_ = ntohs(addr.sin_port);
    if (::listen(listenFd_, 1) != 0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return;
    }
    thread_ = std::thread([this] { serveOnce(); });
  }

  ~LegacyEchoServer() {
    if (listenFd_ >= 0) {
      // connect() to our own listener so a thread blocked in accept() wakes
      // up; closing the listening fd alone does not reliably do that.
      const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
      if (wake >= 0) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        ::connect(wake, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        ::close(wake);
      }
      ::close(listenFd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LegacyEchoServer(const LegacyEchoServer &) = delete;
  LegacyEchoServer &operator=(const LegacyEchoServer &) = delete;

  uint16_t port() const { return port_; }

private:
  static bool readExact(int fd, void *data, size_t size) {
    auto *out = static_cast<char *>(data);
    size_t done = 0;
    while (done < size) {
      const ssize_t n = ::recv(fd, out + done, size - done, 0);
      if (n <= 0) {
        return false;
      }
      done += static_cast<size_t>(n);
    }
    return true;
  }

  static bool writeExact(int fd, const void *data, size_t size) {
    const auto *in = static_cast<const char *>(data);
    size_t done = 0;
    while (done < size) {
      const ssize_t n = ::send(fd, in + done, size - done, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      done += static_cast<size_t>(n);
    }
    return true;
  }

  void serveOnce() {
    const int conn = ::accept(listenFd_, nullptr, nullptr);
    if (conn < 0) {
      return;
    }
    // Bound every read so a misbehaving test cannot hang the join() below.
    timeval timeout{5, 0};
    ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char header[kHeaderSize] = {};
    bool ok = readExact(conn, header, sizeof(header));
    MessageHeader decoded{};
    if (ok) {
      ok = decodeHeader(header, sizeof(header), &decoded) &&
           decoded.bodyLen <= 64;
    }
    std::vector<char> body;
    if (ok) {
      body.resize(decoded.bodyLen);
      ok = readExact(conn, body.data(), body.size());
    }
    if (ok) {
      // A legacy peer knows nothing about the serializer byte: it answers the
      // way it always did, with both reserved bytes zero.
      MessageHeader response{};
      response.magic = kMagic;
      response.version = kVersion;
      response.type = MessageType::kResponse;
      response.status = StatusCode::kOk;
      response.serializerId = 0;
      response.requestId = decoded.requestId;
      response.bodyLen = 1;

      char encoded[kHeaderSize] = {};
      encodeHeader(response, encoded);
      ok = writeExact(conn, encoded, sizeof(encoded)) &&
           writeExact(conn, "x", 1);
    }

    ::shutdown(conn, SHUT_RDWR);
    ::close(conn);
  }

  int listenFd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

TEST(SerializerNegotiation, ClientRejectsAResponseWithADifferentId) {
  LegacyEchoServer server;
  ASSERT_NE(server.port(), 0u);

  EventLoopThread thread;
  EventLoop *loop = thread.startLoop();
  ASSERT_NE(loop, nullptr);

  std::unique_ptr<RpcClient> client;
  auto done = std::make_shared<std::promise<void>>();
  auto ready = done->get_future();
  loop->runInLoop([&, done] {
    client = std::make_unique<RpcClient>(
        loop, InetAddress(server.port(), "127.0.0.1"));
    client->setSerializerId(kSerializerJson); // we ask for JSON
    done->set_value();
  });
  ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  client->connect();
  const Response response = client->Call(Request{"Echo", "Echo", "{}"});

  // The legacy peer answered raw (id 0). Call() must not return that payload
  // as a successful JSON response: the mismatch closes the connection, which
  // #39's error model reports to outstanding calls as kConnectionClosed.
  EXPECT_EQ(response.status, StatusCode::kConnectionClosed);
  EXPECT_EQ(response.payload, "");

  auto cleanupDone = std::make_shared<std::promise<void>>();
  auto finished = cleanupDone->get_future();
  loop->runInLoop([&, cleanupDone] {
    client.reset();
    cleanupDone->set_value();
  });
  finished.wait_for(std::chrono::seconds(5));
}

} // namespace
