#include <gtest/gtest.h>

#include "Buffer.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

using namespace reactornet;
using namespace reactornet::rpc;

// Tests for the RFC-0001 request/response envelope and the replaceable Codec
// interface (#29). Layer boundary: Codec owns the body, FrameReader owns the
// frame; one case checks the two compose.
namespace {

std::string u16le(uint16_t value) {
  return std::string{static_cast<char>(value & 0xFFu),
                     static_cast<char>((value >> 8) & 0xFFu)};
}

// Build a body the way a buggy or hostile peer might: the caller controls the
// declared lengths independently of the bytes that follow.
std::string rawRequest(const std::string &service, const std::string &method,
                       const std::string &payload = {}) {
  return u16le(static_cast<uint16_t>(service.size())) + service +
         u16le(static_cast<uint16_t>(method.size())) + method + payload;
}

Request sentinelRequest() {
  return Request{"sentinel-service", "sentinel-method", "sentinel-payload"};
}

} // namespace

TEST(RawCodecTest, RequestEnvelopeIsExactlyTheRfcLayout) {
  RawCodec codec;
  Request request{"svc", "m", "P"};

  std::string encoded;
  ASSERT_TRUE(codec.encodeRequest(request, &encoded));
  EXPECT_EQ(encoded, std::string("\x03\x00svc\x01\x00mP", 9));
}

TEST(RawCodecTest, RequestRoundTripPreservesEveryField) {
  RawCodec codec;
  Request in{"UserService", "GetUser", R"({"id":42})"};

  std::string encoded;
  ASSERT_TRUE(codec.encodeRequest(in, &encoded));

  Request out;
  ASSERT_TRUE(codec.decodeRequest(encoded, &out));
  EXPECT_EQ(out.service, in.service);
  EXPECT_EQ(out.method, in.method);
  EXPECT_EQ(out.payload, in.payload);
}

TEST(RawCodecTest, Utf8ServiceAndMethodRoundTripAsByteLengths) {
  RawCodec codec;
  // 4 characters, 3 bytes each in UTF-8: the prefix counts bytes, not glyphs.
  Request in{"用户服务", "获取用户", "payload"};

  std::string encoded;
  ASSERT_TRUE(codec.encodeRequest(in, &encoded));
  ASSERT_GE(encoded.size(), 4u);
  EXPECT_EQ(encoded.substr(0, 2), u16le(12));
  EXPECT_EQ(encoded.substr(2, 12), "用户服务");
  EXPECT_EQ(encoded.substr(14, 2), u16le(12));

  Request out;
  ASSERT_TRUE(codec.decodeRequest(encoded, &out));
  EXPECT_EQ(out.service, in.service);
  EXPECT_EQ(out.method, in.method);
  EXPECT_EQ(out.payload, in.payload);
}

TEST(RawCodecTest, EmptyServiceOrMethodIsRejected) {
  RawCodec codec;
  std::string encoded = "untouched";

  EXPECT_FALSE(codec.encodeRequest(Request{"", "m", ""}, &encoded));
  EXPECT_EQ(encoded, "untouched");
  EXPECT_FALSE(codec.encodeRequest(Request{"s", "", ""}, &encoded));
  EXPECT_EQ(encoded, "untouched");

  Request out = sentinelRequest();

  // A peer can send svc_len = 0 even though our encoder never would.
  EXPECT_FALSE(codec.decodeRequest(rawRequest("", "m"), &out));
  EXPECT_FALSE(codec.decodeRequest(rawRequest("s", ""), &out));
  EXPECT_EQ(out.service, "sentinel-service");
  EXPECT_EQ(out.method, "sentinel-method");
  EXPECT_EQ(out.payload, "sentinel-payload");
}

TEST(RawCodecTest, MalformedLengthPrefixesAreRejectedWithoutTouchingTheOutput) {
  RawCodec codec;
  Request out = sentinelRequest();

  // Nothing to read a prefix from.
  EXPECT_FALSE(codec.decodeRequest("", &out));
  EXPECT_FALSE(codec.decodeRequest(std::string_view("\x05", 1), &out));

  // svc_len promises 5 bytes but only 3 follow.
  EXPECT_FALSE(
      codec.decodeRequest(std::string_view("\x05\x00" "abc", 5), &out));

  // svc_len fits but the method_len field itself is cut in half.
  EXPECT_FALSE(
      codec.decodeRequest(std::string_view("\x03\x00" "abc\x01", 6), &out));

  // method_len promises 5 bytes but only 2 follow.
  EXPECT_FALSE(codec.decodeRequest(
      std::string_view("\x03\x00" "abc\x05\x00" "xy", 10), &out));

  EXPECT_EQ(out.service, "sentinel-service");
  EXPECT_EQ(out.method, "sentinel-method");
  EXPECT_EQ(out.payload, "sentinel-payload");
}

TEST(RawCodecTest, EveryTruncatedPrefixOfTheEnvelopeIsRejected) {
  RawCodec codec;
  const std::string envelope = rawRequest("service", "method"); // no payload

  for (size_t len = 0; len < envelope.size(); ++len) {
    Request out = sentinelRequest();
    EXPECT_FALSE(codec.decodeRequest(
        std::string_view(envelope.data(), len), &out))
        << "a " << len << "-byte prefix must not decode";
    EXPECT_EQ(out.service, "sentinel-service");
  }
}

TEST(RawCodecTest, LengthPrefixBoundaryIs65535) {
  RawCodec codec;

  Request atLimit{std::string(65535, 's'), "m", "p"};
  std::string encoded;
  ASSERT_TRUE(codec.encodeRequest(atLimit, &encoded));
  EXPECT_EQ(encoded.substr(0, 2), u16le(65535));

  Request decoded;
  ASSERT_TRUE(codec.decodeRequest(encoded, &decoded));
  EXPECT_EQ(decoded.service.size(), 65535u);
  EXPECT_EQ(decoded.service, atLimit.service);
  EXPECT_EQ(decoded.method, "m");
  EXPECT_EQ(decoded.payload, "p");

  // One byte more cannot be expressed in a u16 prefix.
  std::string untouched = "untouched";
  EXPECT_FALSE(
      codec.encodeRequest(Request{std::string(65536, 's'), "m", ""}, &untouched));
  EXPECT_EQ(untouched, "untouched");
  EXPECT_FALSE(
      codec.encodeRequest(Request{"s", std::string(65536, 'm'), ""}, &untouched));
  EXPECT_EQ(untouched, "untouched");
}

TEST(RawCodecTest, OneMiBPayloadRoundTrips) {
  RawCodec codec;
  std::string payload(1024u * 1024u, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>((i * 7u) & 0xFFu);
  }

  Request in{"Svc", "Method", payload};
  std::string encoded;
  ASSERT_TRUE(codec.encodeRequest(in, &encoded));

  Request out;
  ASSERT_TRUE(codec.decodeRequest(encoded, &out));
  EXPECT_EQ(out.payload.size(), payload.size());
  EXPECT_EQ(out.payload, payload);
}

TEST(RawCodecTest, ResponseBodyIsPayloadOnlyAndStatusIsNotSerialized) {
  RawCodec codec;
  Response in;
  in.status = StatusCode::kInternalError;
  in.payload = "boom";

  std::string encoded;
  ASSERT_TRUE(codec.encodeResponse(in, &encoded));
  EXPECT_EQ(encoded, "boom") << "status belongs to the frame header, not body";

  Response out;
  out.status = StatusCode::kInternalError; // caller's value from the header
  ASSERT_TRUE(codec.decodeResponse(encoded, &out));
  EXPECT_EQ(out.payload, "boom");
  EXPECT_EQ(out.status, StatusCode::kInternalError)
      << "decodeResponse must not invent a status";

  Response empty;
  std::string emptyEncoded;
  ASSERT_TRUE(codec.encodeResponse(empty, &emptyEncoded));
  EXPECT_EQ(emptyEncoded, "");
  Response emptyOut;
  ASSERT_TRUE(codec.decodeResponse(emptyEncoded, &emptyOut));
  EXPECT_EQ(emptyOut.payload, "");
}

TEST(RawCodecTest, EncodedBodiesFitTheFrameLayer) {
  RawCodec codec;
  Request in{"UserService", "GetUser", R"({"id":42})"};

  std::string body;
  ASSERT_TRUE(codec.encodeRequest(in, &body));

  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kRequest;
  header.status = StatusCode::kOk;
  header.requestId = 1;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encodedHeader{};
  encodeHeader(header, encodedHeader.data());

  Buffer buf;
  buf.append(encodedHeader.data(), encodedHeader.size());
  buf.append(body);

  FrameReader reader;
  MessageHeader parsed{};
  std::string parsedBody;
  ASSERT_EQ(reader.parse(&buf, &parsed, &parsedBody), ParseResult::kOk);
  EXPECT_EQ(parsed.bodyLen, body.size());
  EXPECT_EQ(buf.readableBytes(), 0u);

  Request out;
  ASSERT_TRUE(codec.decodeRequest(parsedBody, &out));
  EXPECT_EQ(out.service, in.service);
  EXPECT_EQ(out.method, in.method);
  EXPECT_EQ(out.payload, in.payload);
}
