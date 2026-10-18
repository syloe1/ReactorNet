#include "rpc/json_serializer.h"

#include "rpc/json_writer.h"
#include "rpc/protocol.h"

namespace reactornet::rpc {

JsonSerializer::JsonSerializer(size_t maxDepth) : maxDepth_(maxDepth) {}

std::string JsonSerializer::name() const { return "json"; }

uint8_t JsonSerializer::id() const { return kSerializerJson; }

bool JsonSerializer::encode(const JsonValue &value, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  std::string encoded;
  if (!JsonWriter(maxDepth_).write(value, &encoded)) {
    return false;
  }
  if (encoded.size() > kMaxBodyLen) {
    return false;
  }
  out->swap(encoded);
  return true;
}

bool JsonSerializer::decode(std::string_view in, JsonValue *out) {
  if (out == nullptr || in.size() > kMaxBodyLen) {
    return false;
  }
  return JsonParser(maxDepth_).parse(in, out);
}

} // namespace reactornet::rpc
