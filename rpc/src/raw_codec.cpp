#include "rpc/codec.h"

#include <cstdint>
#include <limits>

namespace reactornet::rpc {

namespace {

constexpr size_t kMaxNameLen = std::numeric_limits<uint16_t>::max();

void putU16(std::string *out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xFFu));
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
}

uint16_t getU16(std::string_view in, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint8_t>(in[offset])) |
         static_cast<uint16_t>(static_cast<uint8_t>(in[offset + 1]) << 8);
}

bool nameFits(const std::string &name) {
  return !name.empty() && name.size() <= kMaxNameLen;
}

} // namespace

bool RawCodec::encodeRequest(const Request &request, std::string *out) {
  if (out == nullptr || !nameFits(request.service) ||
      !nameFits(request.method)) {
    return false;
  }

  const size_t bodyLen = 2 + request.service.size() + 2 +
                         request.method.size() + request.payload.size();
  if (bodyLen > kMaxBodyLen) {
    return false;
  }

  std::string encoded;
  encoded.reserve(bodyLen);
  putU16(&encoded, static_cast<uint16_t>(request.service.size()));
  encoded += request.service;
  putU16(&encoded, static_cast<uint16_t>(request.method.size()));
  encoded += request.method;
  encoded += request.payload;
  *out = std::move(encoded);
  return true;
}

bool RawCodec::decodeRequest(std::string_view in, Request *out) {
  if (out == nullptr || in.size() < 2 || in.size() > kMaxBodyLen) {
    return false;
  }

  const size_t svcLen = getU16(in, 0);
  if (svcLen == 0) {
    return false;
  }
  // Check the prefix fits before touching the bytes it names: a length prefix
  // is data from the peer, never a reason to read or allocate past `in`.
  if (in.size() < 2 + svcLen + 2) {
    return false;
  }

  const size_t methodOffset = 2 + svcLen;
  const size_t methodLen = getU16(in, methodOffset);
  if (methodLen == 0) {
    return false;
  }
  if (in.size() < 4 + svcLen + methodLen) {
    return false;
  }

  Request decoded;
  decoded.service.assign(in.data() + 2, svcLen);
  decoded.method.assign(in.data() + methodOffset + 2, methodLen);
  // The envelope must consume the body exactly: whatever remains is payload.
  decoded.payload.assign(in.data() + 4 + svcLen + methodLen,
                         in.size() - (4 + svcLen + methodLen));
  *out = std::move(decoded);
  return true;
}

bool RawCodec::encodeResponse(const Response &response, std::string *out) {
  if (out == nullptr || response.payload.size() > kMaxBodyLen) {
    return false;
  }
  // response.status belongs to the frame header and is deliberately dropped.
  *out = response.payload;
  return true;
}

bool RawCodec::decodeResponse(std::string_view in, Response *out) {
  if (out == nullptr || in.size() > kMaxBodyLen) {
    return false;
  }
  // The body is the payload verbatim. `status` is not in the body: the caller
  // takes it from the frame header and leaves it as it is here.
  out->payload.assign(in.data(), in.size());
  return true;
}

} // namespace reactornet::rpc
