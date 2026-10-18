#include "rpc/json_writer.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace reactornet::rpc {

namespace {

// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
// values above U+10FFFF, stray continuation bytes and truncated sequences.
// JSON text must be valid UTF-8, so a JsonValue carrying arbitrary bytes (the
// raw serializer allows that) cannot be encoded as JSON.
bool isValidUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }

    size_t length = 0;
    uint32_t codePoint = 0;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      codePoint = lead & 0x1F;
      if (codePoint < 2) {
        return false; // overlong (0xC0/0xC1)
      }
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      codePoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      codePoint = lead & 0x07;
      if (codePoint > 4) {
        return false; // above U+10FFFF
      }
    } else {
      return false; // stray continuation byte or invalid lead
    }

    if (i + length > text.size()) {
      return false;
    }
    for (size_t j = 1; j < length; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (next & 0x3F);
    }

    if ((length == 3 && codePoint < 0x800) ||
        (length == 4 && codePoint < 0x10000)) {
      return false; // overlong
    }
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
      return false; // UTF-16 surrogate, never a valid scalar value
    }
    if (codePoint > 0x10FFFF) {
      return false; // 0xF4 0x90.. would otherwise assemble past the last cp
    }
    i += length;
  }
  return true;
}

bool writeString(std::string_view text, std::string *out) {
  if (!isValidUtf8(text)) {
    return false;
  }

  static constexpr char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '"':
      out->append("\\\"");
      break;
    case '\\':
      out->append("\\\\");
      break;
    case '\b':
      out->append("\\b");
      break;
    case '\f':
      out->append("\\f");
      break;
    case '\n':
      out->append("\\n");
      break;
    case '\r':
      out->append("\\r");
      break;
    case '\t':
      out->append("\\t");
      break;
    default:
      if (c < 0x20) {
        out->append("\\u00");
        out->push_back(kHex[c >> 4]);
        out->push_back(kHex[c & 0x0F]);
      } else {
        // Bytes >= 0x20, including non-ASCII, pass through unchanged: the
        // input was validated as UTF-8 above and JSON accepts raw UTF-8.
        out->push_back(raw);
      }
      break;
    }
  }
  out->push_back('"');
  return true;
}

bool writeNumber(const JsonValue &value, std::string *out) {
  char buffer[64];
  if (value.isInteger()) {
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value.asInt());
    if (result.ec != std::errc()) {
      return false;
    }
    out->append(buffer, result.ptr);
    return true;
  }

  const double number = value.asNumber();
  if (!std::isfinite(number)) {
    return false; // JSON has no NaN/Infinity literal
  }

  // Shortest representation that round-trips to the same double. to_chars is
  // locale-independent, unlike printf("%g"), so a caller in a comma-decimal
  // locale still gets a valid JSON number.
  const auto result =
      std::to_chars(buffer, buffer + sizeof(buffer), number);
  if (result.ec != std::errc()) {
    return false;
  }
  const auto hasFractionOrExponent =
      std::string_view(buffer, static_cast<size_t>(result.ptr - buffer))
          .find_first_of(".eE");
  out->append(buffer, result.ptr);
  if (hasFractionOrExponent == std::string_view::npos) {
    // "1" would parse back as an integer; keep the value's type stable.
    out->append(".0");
  }
  return true;
}

bool writeValue(const JsonValue &value, size_t depth, size_t maxDepth,
                std::string *out) {
  switch (value.type()) {
  case JsonValue::Type::kNull:
    out->append("null");
    return true;
  case JsonValue::Type::kBool:
    out->append(value.asBool() ? "true" : "false");
    return true;
  case JsonValue::Type::kNumber:
    return writeNumber(value, out);
  case JsonValue::Type::kString:
    return writeString(value.asString(), out);
  case JsonValue::Type::kArray: {
    // depth is the number of containers already entered. Entering this one
    // must not exceed maxDepth, which keeps both the writer's recursion and
    // the parser's later recursion bounded by the same number.
    if (depth >= maxDepth) {
      return false;
    }
    const auto &array = value.asArray();
    out->push_back('[');
    for (size_t i = 0; i < array.size(); ++i) {
      if (i != 0) {
        out->push_back(',');
      }
      if (!writeValue(array[i], depth + 1, maxDepth, out)) {
        return false;
      }
    }
    out->push_back(']');
    return true;
  }
  case JsonValue::Type::kObject: {
    if (depth >= maxDepth) {
      return false;
    }
    const auto &object = value.asObject();
    out->push_back('{');
    for (size_t i = 0; i < object.size(); ++i) {
      if (i != 0) {
        out->push_back(',');
      }
      if (!writeString(object[i].first, out)) {
        return false;
      }
      out->push_back(':');
      if (!writeValue(object[i].second, depth + 1, maxDepth, out)) {
        return false;
      }
    }
    out->push_back('}');
    return true;
  }
  }
  return false;
}

} // namespace

bool JsonWriter::write(const JsonValue &value, std::string *out) const {
  if (out == nullptr) {
    return false;
  }
  // Build in a local buffer: a failure part-way through must not leave the
  // caller's output looking like a successful (truncated) payload.
  std::string encoded;
  if (!writeValue(value, 0, maxDepth_, &encoded)) {
    return false;
  }
  out->swap(encoded);
  return true;
}

std::string JsonWriter::write(const JsonValue &value) const {
  std::string encoded;
  if (!write(value, &encoded)) {
    return {};
  }
  return encoded;
}

} // namespace reactornet::rpc
