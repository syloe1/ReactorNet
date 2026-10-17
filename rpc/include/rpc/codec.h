#pragma once

#include "rpc/protocol.h"

#include <string>
#include <string_view>

namespace reactornet::rpc {

// Message semantics, layer 2 of the RPC stack: an envelope naming what to call,
// wrapped around an opaque payload. Serialisation of the payload is a separate
// concern (#42/#43) so the RPC machinery can work before any serializer exists.
//
// RFC-0001 §4.3 fixes the body layouts:
//
//   REQUEST   [u16 svc_len][service][u16 method_len][method][payload]
//   RESPONSE  [payload]
//
// `status` is *not* part of the body: it travels in the frame header
// (RFC-0001 §4.2) and is only mirrored here so a Response can carry the result
// and its payload together. A Codec never serialises or parses it.

struct Request {
  std::string service;
  std::string method;
  std::string payload; // v0.1: opaque bytes
};

struct Response {
  StatusCode status = StatusCode::kOk; // frame-header field, not body
  std::string payload;                 // v0.1: opaque bytes
};

// A replaceable body codec. Making this an interface rather than free
// functions is what lets the M5 serialisers (JSON/protobuf) drop in without
// touching RpcServer/RpcClient.
class Codec {
public:
  virtual ~Codec() = default;

  virtual bool encodeRequest(const Request &request, std::string *out) = 0;
  virtual bool decodeRequest(std::string_view in, Request *out) = 0;
  virtual bool encodeResponse(const Response &response, std::string *out) = 0;
  virtual bool decodeResponse(std::string_view in, Response *out) = 0;
};

// The v0.1 codec: envelope fields are u16 length-prefixed UTF-8, the payload
// is copied through untouched.
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
