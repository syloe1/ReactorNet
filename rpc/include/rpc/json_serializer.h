#pragma once

#include "rpc/json_parser.h"
#include "rpc/serializer.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace reactornet::rpc {

// The reference payload serializer (#43): JSON text in the frame body, id 1
// in the header. It is a thin adapter over JsonWriter / JsonParser so the
// text format stays independently testable.
//
// maxDepth applies to both directions and defaults to JsonParser's limit, so
// encode and decode agree on what "too deep" means.
class JsonSerializer : public Serializer {
public:
  explicit JsonSerializer(size_t maxDepth = JsonParser::kDefaultMaxDepth);

  std::string name() const override;
  uint8_t id() const override;

  // Fails for values JSON cannot represent (non-finite number, invalid UTF-8,
  // nesting too deep) and for output above the protocol's body ceiling.
  bool encode(const JsonValue &value, std::string *out) override;

  // Rejects malformed input, invalid UTF-8, over-deep nesting and bodies above
  // the protocol's ceiling. On failure *out is untouched.
  bool decode(std::string_view in, JsonValue *out) override;

private:
  size_t maxDepth_;
};

} // namespace reactornet::rpc
