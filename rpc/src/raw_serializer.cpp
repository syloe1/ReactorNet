#include "rpc/serializer.h"

#include "rpc/protocol.h"

namespace reactornet::rpc {

std::string RawSerializer::name() const { return "raw"; }

uint8_t RawSerializer::id() const { return kSerializerRaw; }

bool RawSerializer::encode(const JsonValue &value, std::string *out) {
  if (out == nullptr || !value.isString()) {
    return false;
  }
  if (value.asString().size() > kMaxBodyLen) {
    return false;
  }
  *out = value.asString();
  return true;
}

bool RawSerializer::decode(std::string_view in, JsonValue *out) {
  if (out == nullptr || in.size() > kMaxBodyLen) {
    return false;
  }
  // std::string(in) keeps embedded NULs: this is a byte string, not text.
  *out = JsonValue(std::string(in));
  return true;
}

} // namespace reactornet::rpc
