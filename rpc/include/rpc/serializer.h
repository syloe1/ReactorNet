#pragma once

#include "rpc/json_value.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace reactornet::rpc {

// Payload half of the RPC stack: JsonValue <-> the payload bytes that Codec
// carries inside the envelope. Keeping this separate from Codec is what lets
// the wire layout (RFC-0001) and the payload format evolve independently
// (#42); JSON is the reference implementation (#43), protobuf an optional
// plugin (ADR-002).
//
// Every serializer has two identifiers:
//   - id() is the single byte written to the frame header, and the value the
//     peer must echo. It is the only thing on the wire.
//   - name() is a stable human-readable label ("raw", "json") for logs and
//     configuration. It must stay consistent with id().
//
// Failure contract, shared by encode/decode: false means the input was not
// accepted. decode must not read outside `in` or allocate from lengths inside
// it before those lengths have been checked; encode must not leave *out in a
// partially written state callers could mistake for success.
class Serializer {
public:
  virtual ~Serializer() = default;

  virtual std::string name() const = 0;
  virtual uint8_t id() const = 0;

  virtual bool encode(const JsonValue &value, std::string *out) = 0;
  virtual bool decode(std::string_view in, JsonValue *out) = 0;
};

// The raw serializer: the payload is a byte string, carried verbatim.
//
// Its object model is a single JsonValue string. The bytes may be arbitrary --
// including NUL and sequences that are not valid UTF-8 -- because this format
// makes no text guarantee at all. That is exactly the v0.1 behaviour, which is
// why raw is id 0: it is what every pre-#42 sender wrote in the reserved byte.
class RawSerializer : public Serializer {
public:
  std::string name() const override;
  uint8_t id() const override;

  // Requires a string value: raw has no schema to serialise a number or an
  // object from. Fails (false) for any other type.
  bool encode(const JsonValue &value, std::string *out) override;

  // Accepts any bytes up to the protocol's body ceiling and wraps them as a
  // string value. It never fails for a correctly sized input.
  bool decode(std::string_view in, JsonValue *out) override;
};

} // namespace reactornet::rpc
