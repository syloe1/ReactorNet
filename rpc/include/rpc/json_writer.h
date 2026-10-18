#pragma once

#include "rpc/json_value.h"

#include <cstddef>
#include <string>

namespace reactornet::rpc {

// JsonValue -> JSON text (#43). The writer owns quoting, escaping and the
// depth bound; JsonValue itself has no notion of text.
//
// write() can fail for two reasons that have no JSON spelling:
//   - a non-finite double (NaN or +/-inf);
//   - a string that is not valid UTF-8.
// The bool overload is the authoritative API: on failure *out is untouched, so
// a Serializer can honour its "never leave a partially written payload"
// contract. The string overload returns an empty string on failure; callers
// that need to distinguish failure from `""` must use the bool overload.
//
// Nesting is bounded for the same reason the parser will be: a recursively
// built payload must not be able to overflow the writer's stack.
class JsonWriter {
public:
  static constexpr size_t kDefaultMaxDepth = 64;

  explicit JsonWriter(size_t maxDepth = kDefaultMaxDepth)
      : maxDepth_(maxDepth) {}

  bool write(const JsonValue &value, std::string *out) const;

  std::string write(const JsonValue &value) const;

  size_t maxDepth() const { return maxDepth_; }

private:
  size_t maxDepth_;
};

} // namespace reactornet::rpc
