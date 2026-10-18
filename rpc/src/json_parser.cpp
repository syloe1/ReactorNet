#include "rpc/json_parser.h"

#include "json_utf8.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace reactornet::rpc {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void appendUtf8(uint32_t codePoint, std::string *out) {
  if (codePoint <= 0x7F) {
    out->push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

// One parse, one cursor. The public JsonParser is stateless so it can be
// reused concurrently; all mutable state lives here.
class Reader {
public:
  Reader(std::string_view input, size_t maxDepth)
      : in_(input), maxDepth_(maxDepth) {}

  bool parse(JsonValue *out) {
    skipWhitespace();
    JsonValue value;
    if (!parseValue(0, &value)) {
      return false;
    }
    skipWhitespace();
    if (pos_ != in_.size()) {
      return false; // trailing garbage
    }
    *out = std::move(value);
    return true;
  }

private:
  void skipWhitespace() {
    while (pos_ < in_.size()) {
      const char c = in_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return;
      }
      ++pos_;
    }
  }

  bool consume(std::string_view literal) {
    if (in_.substr(pos_, literal.size()) != literal) {
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  bool parseValue(size_t depth, JsonValue *out) {
    if (pos_ >= in_.size()) {
      return false;
    }
    switch (in_[pos_]) {
    case 'n':
      if (!consume("null")) {
        return false;
      }
      *out = JsonValue();
      return true;
    case 't':
      if (!consume("true")) {
        return false;
      }
      *out = JsonValue::makeBool(true);
      return true;
    case 'f':
      if (!consume("false")) {
        return false;
      }
      *out = JsonValue::makeBool(false);
      return true;
    case '"': {
      std::string text;
      if (!parseString(&text)) {
        return false;
      }
      *out = JsonValue(std::move(text));
      return true;
    }
    case '[':
      return parseArray(depth, out);
    case '{':
      return parseObject(depth, out);
    default:
      return parseNumber(out);
    }
  }

  bool parseNumber(JsonValue *out) {
    const size_t start = pos_;
    if (pos_ < in_.size() && in_[pos_] == '-') {
      ++pos_;
    }
    if (pos_ >= in_.size()) {
      return false;
    }

    if (in_[pos_] == '0') {
      ++pos_;
      if (pos_ < in_.size() && isDigit(in_[pos_])) {
        return false; // leading zero
      }
    } else if (in_[pos_] >= '1' && in_[pos_] <= '9') {
      while (pos_ < in_.size() && isDigit(in_[pos_])) {
        ++pos_;
      }
    } else {
      return false;
    }

    bool isFloating = false;
    if (pos_ < in_.size() && in_[pos_] == '.') {
      isFloating = true;
      ++pos_;
      if (pos_ >= in_.size() || !isDigit(in_[pos_])) {
        return false;
      }
      while (pos_ < in_.size() && isDigit(in_[pos_])) {
        ++pos_;
      }
    }
    if (pos_ < in_.size() && (in_[pos_] == 'e' || in_[pos_] == 'E')) {
      isFloating = true;
      ++pos_;
      if (pos_ < in_.size() && (in_[pos_] == '+' || in_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= in_.size() || !isDigit(in_[pos_])) {
        return false;
      }
      while (pos_ < in_.size() && isDigit(in_[pos_])) {
        ++pos_;
      }
    }

    const char *first = in_.data() + start;
    const char *last = in_.data() + pos_;
    if (!isFloating) {
      int64_t integer = 0;
      const auto result = std::from_chars(first, last, integer);
      if (result.ec == std::errc() && result.ptr == last) {
        *out = JsonValue::makeInteger(integer);
        return true;
      }
      // Out of int64 range: fall through and keep it as a double rather than
      // rejecting a syntactically valid JSON number.
    }

    double number = 0.0;
    const auto result = std::from_chars(first, last, number);
    if (result.ec != std::errc() || result.ptr != last ||
        !std::isfinite(number)) {
      return false; // includes overflow to infinity and underflow to zero
    }
    *out = JsonValue::makeNumber(number);
    return true;
  }

  bool readHex4(uint32_t *out) {
    if (pos_ + 4 > in_.size()) {
      return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hexDigit(in_[pos_ + static_cast<size_t>(i)]);
      if (digit < 0) {
        return false;
      }
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  bool parseUnicodeEscape(std::string *out) {
    uint32_t codePoint = 0;
    if (!readHex4(&codePoint)) {
      return false;
    }

    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
      // High surrogate: JSON's \u escapes are UTF-16 code units, so a pair is
      // required. A lone high surrogate is not a scalar value and cannot be
      // encoded.
      if (pos_ + 2 > in_.size() || in_[pos_] != '\\' || in_[pos_ + 1] != 'u') {
        return false;
      }
      pos_ += 2;
      uint32_t low = 0;
      if (!readHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
        return false;
      }
      codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
    } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
      return false; // lone low surrogate
    }

    appendUtf8(codePoint, out);
    return true;
  }

  bool parseEscape(std::string *out) {
    if (pos_ >= in_.size()) {
      return false;
    }
    switch (in_[pos_++]) {
    case '"':
      out->push_back('"');
      return true;
    case '\\':
      out->push_back('\\');
      return true;
    case '/':
      out->push_back('/');
      return true;
    case 'b':
      out->push_back('\b');
      return true;
    case 'f':
      out->push_back('\f');
      return true;
    case 'n':
      out->push_back('\n');
      return true;
    case 'r':
      out->push_back('\r');
      return true;
    case 't':
      out->push_back('\t');
      return true;
    case 'u':
      return parseUnicodeEscape(out);
    default:
      return false;
    }
  }

  bool parseString(std::string *out) {
    if (pos_ >= in_.size() || in_[pos_] != '"') {
      return false;
    }
    ++pos_;

    std::string text;
    while (pos_ < in_.size()) {
      const auto c = static_cast<unsigned char>(in_[pos_++]);
      if (c == '"') {
        // Validates raw multi-byte sequences in one pass; escaped code points
        // were already encoded as valid UTF-8 above.
        if (!detail::isValidUtf8(text)) {
          return false;
        }
        out->swap(text);
        return true;
      }
      if (c == '\\') {
        if (!parseEscape(&text)) {
          return false;
        }
        continue;
      }
      if (c < 0x20) {
        return false; // unescaped control character
      }
      text.push_back(static_cast<char>(c));
    }
    return false; // unterminated
  }

  bool parseArray(size_t depth, JsonValue *out) {
    if (depth >= maxDepth_) {
      return false; // entering this container would exceed the limit
    }
    ++pos_; // '['
    JsonValue array = JsonValue::makeArray();

    skipWhitespace();
    if (pos_ < in_.size() && in_[pos_] == ']') {
      ++pos_;
      *out = std::move(array);
      return true;
    }

    while (true) {
      skipWhitespace();
      JsonValue element;
      if (!parseValue(depth + 1, &element)) {
        return false;
      }
      array.asArray().push_back(std::move(element));

      skipWhitespace();
      if (pos_ >= in_.size()) {
        return false;
      }
      if (in_[pos_] == ',') {
        ++pos_;
        continue; // a trailing comma fails in parseValue above
      }
      if (in_[pos_] == ']') {
        ++pos_;
        *out = std::move(array);
        return true;
      }
      return false;
    }
  }

  bool parseObject(size_t depth, JsonValue *out) {
    if (depth >= maxDepth_) {
      return false;
    }
    ++pos_; // '{'
    JsonValue object = JsonValue::makeObject();

    skipWhitespace();
    if (pos_ < in_.size() && in_[pos_] == '}') {
      ++pos_;
      *out = std::move(object);
      return true;
    }

    while (true) {
      skipWhitespace();
      std::string key;
      if (!parseString(&key)) {
        return false;
      }
      skipWhitespace();
      if (pos_ >= in_.size() || in_[pos_] != ':') {
        return false;
      }
      ++pos_;
      skipWhitespace();

      JsonValue value;
      if (!parseValue(depth + 1, &value)) {
        return false;
      }
      object.asObject().emplace_back(std::move(key), std::move(value));

      skipWhitespace();
      if (pos_ >= in_.size()) {
        return false;
      }
      if (in_[pos_] == ',') {
        ++pos_;
        continue; // a trailing comma fails when the next key is read
      }
      if (in_[pos_] == '}') {
        ++pos_;
        *out = std::move(object);
        return true;
      }
      return false;
    }
  }

  std::string_view in_;
  size_t pos_ = 0;
  size_t maxDepth_;
};

} // namespace

bool JsonParser::parse(std::string_view in, JsonValue *out) const {
  if (out == nullptr) {
    return false;
  }
  Reader reader(in, maxDepth_);
  JsonValue value;
  if (!reader.parse(&value)) {
    return false;
  }
  *out = std::move(value);
  return true;
}

} // namespace reactornet::rpc
