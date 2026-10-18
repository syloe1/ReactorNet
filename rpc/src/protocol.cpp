#include "rpc/protocol.h"

namespace reactornet::rpc {

const char *statusToString(StatusCode status) {
  switch (status) {
  case StatusCode::kOk:
    return "OK";
  case StatusCode::kUnsupportedVersion:
    return "UNSUPPORTED_VERSION";
  case StatusCode::kUnsupportedMsgType:
    return "UNSUPPORTED_MSG_TYPE";
  case StatusCode::kUnsupportedFlag:
    return "UNSUPPORTED_FLAG";
  case StatusCode::kBadRequestId:
    return "BAD_REQUEST_ID";
  case StatusCode::kBodyTooLarge:
    return "BODY_TOO_LARGE";
  case StatusCode::kMalformedBody:
    return "MALFORMED_BODY";
  case StatusCode::kUnknownService:
    return "UNKNOWN_SERVICE";
  case StatusCode::kUnknownMethod:
    return "UNKNOWN_METHOD";
  case StatusCode::kInternalError:
    return "INTERNAL_ERROR";
  case StatusCode::kTimeout:
    return "TIMEOUT";
  case StatusCode::kConnectionClosed:
    return "CONNECTION_CLOSED";
  case StatusCode::kBadRequest:
    return "BAD_REQUEST";
  }
  return "UNKNOWN_STATUS";
}

static_assert(kHeaderSize == 16, "RFC-0001 fixes the frame header at 16 bytes");

namespace {

// Byte-wise helpers. Shifting unsigned values avoids sign-extension and does
// not assume the host is little-endian: the wire order is written out
// explicitly, independently of how the machine stores its integers.
void putU16(char *out, uint16_t value) {
  out[0] = static_cast<char>(value & 0xFFu);
  out[1] = static_cast<char>((value >> 8) & 0xFFu);
}

void putU32(char *out, uint32_t value) {
  out[0] = static_cast<char>(value & 0xFFu);
  out[1] = static_cast<char>((value >> 8) & 0xFFu);
  out[2] = static_cast<char>((value >> 16) & 0xFFu);
  out[3] = static_cast<char>((value >> 24) & 0xFFu);
}

uint16_t getU16(const char *in) {
  return static_cast<uint16_t>(static_cast<uint8_t>(in[0])) |
         static_cast<uint16_t>(static_cast<uint8_t>(in[1]) << 8);
}

uint32_t getU32(const char *in) {
  return static_cast<uint32_t>(static_cast<uint8_t>(in[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[3])) << 24);
}

} // namespace

void encodeHeader(const MessageHeader &header, char *out) {
  putU16(out, header.magic);
  out[2] = static_cast<char>(header.version);
  out[3] = static_cast<char>(header.type);
  out[4] = static_cast<char>(header.flags);
  out[5] = static_cast<char>(header.status);
  out[6] = static_cast<char>(header.serializerId);
  // reserved: RFC-0001 §4.2 requires senders to write zero.
  out[7] = 0;
  putU32(out + 8, header.requestId);
  putU32(out + 12, header.bodyLen);
}

bool decodeHeader(const char *in, size_t len, MessageHeader *out) {
  if (in == nullptr || out == nullptr || len < kHeaderSize) {
    return false;
  }

  // Magic is the only framing check this layer owns. Do it before touching
  // *out so a rejected buffer leaves the caller's value untouched.
  const uint16_t magic = getU16(in);
  if (magic != kMagic) {
    return false;
  }

  out->magic = magic;
  out->version = static_cast<uint8_t>(in[2]);
  out->type = static_cast<MessageType>(in[3]);
  out->flags = static_cast<uint8_t>(in[4]);
  out->status = static_cast<StatusCode>(in[5]);
  out->serializerId = static_cast<uint8_t>(in[6]);
  // in[7] is reserved: deliberately ignored, as RFC-0001 §8 requires.
  out->requestId = getU32(in + 8);
  out->bodyLen = getU32(in + 12);
  return true;
}

} // namespace reactornet::rpc
