#pragma once

#include <cstddef>
#include <cstdint>

namespace reactornet::rpc {

// Wire protocol v0.1, as specified by RFC-0001 (docs/rfc/0001-rpc-protocol.md).
//
// A frame is a fixed 16-byte header followed by bodyLen opaque body bytes:
//
//   offset  size  field
//   0       2     magic      0x4E52, i.e. the bytes 52 4E ("RN")
//   2       1     version    1
//   3       1     type       1 = REQUEST, 2 = RESPONSE
//   4       1     flags
//   5       1     status
//   6       2     reserved   MUST be 0 on send, ignored on receive
//   8       4     requestId  non-zero; echoed by the response
//   12      4     bodyLen
//
// Every multi-byte integer is little-endian and there is no alignment padding
// on the wire. The struct below is only an in-memory value: these functions
// write and read each byte explicitly, never memcpy the struct (RFC-0001 §4,
// §9.2). The decoder validates framing (size and magic) only; policy checks
// such as version, flags and bodyLen limits belong to the layer above (#28).

enum class MessageType : uint8_t { kRequest = 1, kResponse = 2 };

// Values from RFC-0001 §4.6. 0x0A-0x7F are reserved (a receiver treats an
// unknown response status as failure, RFC-0001 §8); 0x80-0xFF are
// application-defined.
enum class StatusCode : uint8_t {
  kOk = 0x00,
  kUnsupportedVersion = 0x01,
  kUnsupportedMsgType = 0x02,
  kUnsupportedFlag = 0x03,
  kBadRequestId = 0x04,
  kBodyTooLarge = 0x05,
  kMalformedBody = 0x06,
  kUnknownService = 0x07,
  kUnknownMethod = 0x08,
  kInternalError = 0x09,
  // Client-side deadline expired (#38). A server never sends this status; it
  // exists so callers get a typed result instead of a generic error.
  kTimeout = 0x0A,
};

struct MessageHeader {
  uint16_t magic; // 0x4E52
  uint8_t version; // 1
  MessageType type;
  uint8_t flags;
  StatusCode status;
  // Wire bytes 6-7 are `reserved`: senders MUST write zero, receivers MUST
  // ignore the value, so it has no representation here.
  uint32_t requestId;
  uint32_t bodyLen;
};

constexpr uint16_t kMagic = 0x4E52;
constexpr uint8_t kVersion = 0x01;
constexpr size_t kHeaderSize = 16;

// RFC-0001 §4.5. v0.1 senders write neither bit; a receiver rejects them.
constexpr uint8_t kFlagCompressed = 0x01;
constexpr uint8_t kFlagOneway = 0x02;

// RFC-0001 §4.4: every implementation MUST accept at least 64 KiB. The
// reference implementation's ceiling is 16 MiB, enforced by the frame reader
// (#28) *before* it allocates anything for the body.
constexpr uint32_t kMaxBodyLen = 16u * 1024u * 1024u;

// Writes exactly kHeaderSize bytes to `out`. Pure function: no validation and
// no allocation. reserved is always written as zero.
void encodeHeader(const MessageHeader &header, char *out);

// Reads exactly kHeaderSize leading bytes from `in`, of which `len` are
// readable. Returns false if the input is truncated or the magic does not
// match; on success every field is filled in. On failure `out` is untouched.
// This is a pure codec: version, type, flags, status and bodyLen are decoded
// as-is, not judged.
bool decodeHeader(const char *in, size_t len, MessageHeader *out);

} // namespace reactornet::rpc
