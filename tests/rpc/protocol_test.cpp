#include <gtest/gtest.h>

#include "rpc/protocol.h"

#include <array>
#include <cstdint>
#include <string>

using namespace reactornet::rpc;

// Tests for the RFC-0001 frame header codec (#27).
//
// The primary evidence is the normative golden vectors from RFC-0001 §5:
// hand-built bytes are decoded field by field, then re-encoded and compared
// with the same bytes. A round-trip test alone is blind to a mistake made
// symmetrically in encode and decode, so it is only the auxiliary case here.
namespace {

// RFC-0001 §5.1 — 47-byte GetUser REQUEST. The string concatenation keeps the
// escapes away from following hex digits; the length is checked in the test.
constexpr char kGoldenRequest[] =
    "\x52\x4e\x01\x01\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x1f\x00\x00\x00"
    "\x0b\x00"
    "UserService"
    "\x07\x00"
    "GetUser"
    "{\"id\":42}";

// RFC-0001 §5.2 — 40-byte RESPONSE / OK, request_id 1, 24-byte payload.
constexpr char kGoldenResponse[] =
    "\x52\x4e\x01\x02\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x18\x00\x00\x00"
    "{\"id\":42,\"name\":\"alice\"}";

// RFC-0001 §5.3 — 41-byte RESPONSE / UNSUPPORTED_VERSION, request_id 7.
constexpr char kGoldenUnsupportedVersion[] =
    "\x52\x4e\x01\x02\x00\x01\x00\x00"
    "\x07\x00\x00\x00\x19\x00\x00\x00"
    "unsupported version: 0x02";

std::string copyBytes(const char *data, size_t sizeWithTerminator) {
  return std::string(data, sizeWithTerminator - 1);
}

void expectHeaderEncodesTo(const MessageHeader &header,
                           const std::string &frame) {
  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  EXPECT_EQ(std::string(encoded.data(), encoded.size()),
            frame.substr(0, kHeaderSize));
}

} // namespace

TEST(RpcProtocolTest, ConstantsMatchRfc001) {
  static_assert(kHeaderSize == 16, "RFC-0001 fixes the header at 16 bytes");
  EXPECT_EQ(kMagic, 0x4E52);
  EXPECT_EQ(kVersion, 0x01);
  EXPECT_EQ(static_cast<uint8_t>(MessageType::kRequest), 1);
  EXPECT_EQ(static_cast<uint8_t>(MessageType::kResponse), 2);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kOk), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnsupportedVersion), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnsupportedMsgType), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnsupportedFlag), 0x03);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kBadRequestId), 0x04);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kBodyTooLarge), 0x05);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kMalformedBody), 0x06);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnknownService), 0x07);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnknownMethod), 0x08);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kInternalError), 0x09);
  EXPECT_EQ(static_cast<uint8_t>(StatusCode::kUnsupportedSerializer), 0x0D);
  EXPECT_EQ(kSerializerRaw, 0x00);
  EXPECT_EQ(kSerializerJson, 0x01);
}

TEST(RpcProtocolTest, GoldenRequestHeaderDecodesFieldByField) {
  const std::string frame = copyBytes(kGoldenRequest, sizeof(kGoldenRequest));
  ASSERT_EQ(frame.size(), 47u) << "golden vector transcription is wrong";

  MessageHeader header{};
  ASSERT_TRUE(decodeHeader(frame.data(), frame.size(), &header));

  EXPECT_EQ(header.magic, 0x4E52); // wire bytes 52 4e
  EXPECT_EQ(header.version, 1);
  EXPECT_EQ(header.type, MessageType::kRequest);
  EXPECT_EQ(header.flags, 0);
  EXPECT_EQ(header.status, StatusCode::kOk);
  // Old senders zeroed both reserved bytes; the first one is now the serializer
  // id, so 0 = raw is what every pre-#42 frame decodes to.
  EXPECT_EQ(header.serializerId, kSerializerRaw);
  EXPECT_EQ(header.requestId, 1u);
  EXPECT_EQ(header.bodyLen, 31u);
  // The RFC's self-consistency check: header + body is exactly the frame.
  EXPECT_EQ(kHeaderSize + header.bodyLen, frame.size());

  expectHeaderEncodesTo(header, frame);
}

TEST(RpcProtocolTest, GoldenResponseHeaderDecodesFieldByField) {
  const std::string frame =
      copyBytes(kGoldenResponse, sizeof(kGoldenResponse));
  ASSERT_EQ(frame.size(), 40u) << "golden vector transcription is wrong";

  MessageHeader header{};
  ASSERT_TRUE(decodeHeader(frame.data(), frame.size(), &header));

  EXPECT_EQ(header.magic, 0x4E52);
  EXPECT_EQ(header.version, 1);
  EXPECT_EQ(header.type, MessageType::kResponse);
  EXPECT_EQ(header.flags, 0);
  EXPECT_EQ(header.status, StatusCode::kOk);
  EXPECT_EQ(header.serializerId, kSerializerRaw);
  EXPECT_EQ(header.requestId, 1u);
  EXPECT_EQ(header.bodyLen, 24u);
  EXPECT_EQ(kHeaderSize + header.bodyLen, frame.size());

  expectHeaderEncodesTo(header, frame);
}

TEST(RpcProtocolTest, GoldenErrorResponseHeaderDecodesFieldByField) {
  const std::string frame =
      copyBytes(kGoldenUnsupportedVersion, sizeof(kGoldenUnsupportedVersion));
  ASSERT_EQ(frame.size(), 41u) << "golden vector transcription is wrong";

  MessageHeader header{};
  ASSERT_TRUE(decodeHeader(frame.data(), frame.size(), &header));

  EXPECT_EQ(header.magic, 0x4E52);
  EXPECT_EQ(header.version, 1);
  EXPECT_EQ(header.type, MessageType::kResponse);
  EXPECT_EQ(header.flags, 0);
  EXPECT_EQ(header.status, StatusCode::kUnsupportedVersion);
  EXPECT_EQ(header.serializerId, kSerializerRaw);
  EXPECT_EQ(header.requestId, 7u);
  EXPECT_EQ(header.bodyLen, 25u);
  EXPECT_EQ(kHeaderSize + header.bodyLen, frame.size());

  expectHeaderEncodesTo(header, frame);
}

TEST(RpcProtocolTest, TruncatedHeaderIsRejectedWithoutWritingTheOutput) {
  const std::string frame = copyBytes(kGoldenRequest, sizeof(kGoldenRequest));

  for (size_t len = 0; len < kHeaderSize; ++len) {
    MessageHeader header{};
    header.requestId = 0xA5A5A5A5u; // sentinel: must survive a rejection
    EXPECT_FALSE(decodeHeader(frame.data(), len, &header))
        << "a " << len << "-byte input must not decode";
    EXPECT_EQ(header.requestId, 0xA5A5A5A5u)
        << "a rejected input must leave the output untouched";
  }
}

TEST(RpcProtocolTest, WrongMagicIsRejected) {
  std::string frame = copyBytes(kGoldenRequest, sizeof(kGoldenRequest));
  frame.resize(kHeaderSize);

  frame[0] = '\x53'; // 53 4e
  MessageHeader header{};
  EXPECT_FALSE(decodeHeader(frame.data(), frame.size(), &header));

  frame[0] = '\x52';
  frame[1] = '\x4f'; // 52 4f
  EXPECT_FALSE(decodeHeader(frame.data(), frame.size(), &header));
}

TEST(RpcProtocolTest, SerializerByteIsDecodedAndReservedByteIsIgnored) {
  // RFC-0001 §4.2/§8: byte 6 is the serializer id, byte 7 is still reserved and
  // must be ignored, and undefined flag bits are ignored too. Judging the id
  // belongs to RpcServer/RpcClient (#42), not to this pure codec.
  std::string frame = copyBytes(kGoldenRequest, sizeof(kGoldenRequest));
  frame.resize(kHeaderSize);
  frame[4] = static_cast<char>(0xFC); // bits 2-7
  frame[6] = static_cast<char>(kSerializerJson);
  frame[7] = static_cast<char>(0xBB); // reserved, deliberately garbage

  MessageHeader header{};
  ASSERT_TRUE(decodeHeader(frame.data(), frame.size(), &header));
  EXPECT_EQ(header.flags, 0xFC);
  EXPECT_EQ(header.serializerId, kSerializerJson);
  EXPECT_EQ(header.requestId, 1u);
  EXPECT_EQ(header.bodyLen, 31u);

  // Re-encoding normalises the ignored reserved byte back to zero.
  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  EXPECT_EQ(encoded[6], static_cast<char>(kSerializerJson));
  EXPECT_EQ(encoded[7], '\0');
}

TEST(RpcProtocolTest, RoundTripPreservesEveryField) {
  MessageHeader in{};
  in.magic = kMagic;
  in.version = kVersion;
  in.type = MessageType::kResponse;
  in.flags = 0xF0; // undefined bits pass through
  in.status = StatusCode::kInternalError;
  in.serializerId = kSerializerJson;
  in.requestId = 0xDEADBEEFu;
  in.bodyLen = 0x00012345u;

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(in, encoded.data());

  MessageHeader out{};
  ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), &out));

  EXPECT_EQ(out.magic, in.magic);
  EXPECT_EQ(out.version, in.version);
  EXPECT_EQ(out.type, in.type);
  EXPECT_EQ(out.flags, in.flags);
  EXPECT_EQ(out.status, in.status);
  EXPECT_EQ(out.serializerId, in.serializerId);
  EXPECT_EQ(out.requestId, in.requestId);
  EXPECT_EQ(out.bodyLen, in.bodyLen);
}
