#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace reactornet::rpc::detail {

// Strict UTF-8 validation, shared by the JSON writer and parser: both must
// reject text that JSON cannot represent. Rejects overlong encodings, UTF-16
// surrogate code points, values above U+10FFFF, stray continuation bytes and
// truncated sequences.
inline bool isValidUtf8(std::string_view text) {
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

} // namespace reactornet::rpc::detail
