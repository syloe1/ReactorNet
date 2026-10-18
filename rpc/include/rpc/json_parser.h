#pragma once

#include "rpc/json_value.h"

#include <cstddef>
#include <string_view>

namespace reactornet::rpc {

// JSON text -> JsonValue (#43). A strict recursive-descent parser:
//
//   - only the four RFC 8259 whitespace characters are skipped;
//   - trailing bytes after the root value are an error;
//   - strings validate escapes, survive embedded NUL from \u0000, combine
//     surrogate pairs, and reject invalid UTF-8;
//   - numbers follow the RFC grammar (no leading '+', no leading zeros) and
//     become int64 when they fit, double otherwise;
//   - nesting deeper than maxDepth is rejected before recursing into it, so a
//     hostile `[[[[...` cannot overflow the stack.
//
// parse() leaves *out untouched on failure. It allocates only for the value
// being built; no length inside the input is trusted before it is checked.
class JsonParser {
public:
  static constexpr size_t kDefaultMaxDepth = 64;

  explicit JsonParser(size_t maxDepth = kDefaultMaxDepth)
      : maxDepth_(maxDepth) {}

  bool parse(std::string_view in, JsonValue *out) const;

  size_t maxDepth() const { return maxDepth_; }

private:
  size_t maxDepth_;
};

} // namespace reactornet::rpc
