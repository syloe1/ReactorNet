#pragma once

#include "rpc/protocol.h"

#include <string>
#include <string_view>

namespace reactornet::rpc {

// Message semantics, layer 2 of the RPC stack: an envelope naming what to call,
// wrapped around an opaque payload.
//
// #42 split the two concerns:
//   - Codec (this header) owns the frame + envelope: where the names and the
//     payload bytes sit in the body, and nothing about their meaning.
//   - Serializer (rpc/serializer.h) owns payload <-> object, and its id
//     travels in the frame header so both ends agree on the format.
// The RPC machinery stays usable before any serializer exists: it moves opaque
// bytes either way.
//
// RFC-0001 §4.3 fixes the body layouts:
//
//   REQUEST   [u16 svc_len][service][u16 method_len][method][payload]
//   RESPONSE  [payload]
//
// `status` and `serializerId` are *not* part of the body: they travel in the
// frame header (RFC-0001 §4.2) and are only mirrored here so a Response can
// carry the result and its payload together. A Codec never serialises or
// parses either.

struct Request {
  std::string service;
  std::string method;
  std::string payload; // Serializer output / input; opaque to the Codec
};

struct Response {
  StatusCode status = StatusCode::kOk;   // frame-header field, not body
  std::string payload;                   // Serializer output / input
  uint8_t serializerId = kSerializerRaw; // frame-header field, not body
};

// A replaceable envelope codec. Making this an interface rather than free
// functions is what kept the protocol work independent of the payload format;
// a Serializer can be replaced without touching the wire layout, and vice
// versa.
class Codec {
public:
  virtual ~Codec() = default;

  virtual bool encodeRequest(const Request &request, std::string *out) = 0;
  virtual bool decodeRequest(std::string_view in, Request *out) = 0;
  virtual bool encodeResponse(const Response &response, std::string *out) = 0;
  virtual bool decodeResponse(std::string_view in, Response *out) = 0;
};

// The v0.1 envelope codec: service and method are u16 length-prefixed UTF-8,
// the payload is copied through untouched. It does not parse JSON or any other
// payload format -- that is Serializer's job (rpc/serializer.h).
//
// Failure contract, shared by all four methods: false is returned without
// reading outside `in` and without modifying `*out` (the caller simply drops
// the message). On success the output buffer or request is fully replaced.
//
// Limits:
//   - service and method must be 1..65535 bytes (sentinel lengths are zeroed
//     in the u16 prefix, RFC-0001 §4.3); empty names are a sender bug.
//   - the resulting body must fit kMaxBodyLen (16 MiB), the protocol's
//     absolute ceiling. A peer's configurable frame limit (#28) can be lower.
//   - service/method are identifiers, not paths: v0.1 does not validate their
//     characters, only their lengths (RFC-0001 §4.3).
class RawCodec : public Codec {
public:
  bool encodeRequest(const Request &request, std::string *out) override;
  bool decodeRequest(std::string_view in, Request *out) override;
  bool encodeResponse(const Response &response, std::string *out) override;
  bool decodeResponse(std::string_view in, Response *out) override;
};

} // namespace reactornet::rpc
