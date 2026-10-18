#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reactornet::rpc {

// In-memory payload value shared by every Serializer (rpc/serializer.h).
//
// This is the object model, not JSON text: it has no notion of whitespace,
// escaping or a maximum nesting depth. JsonWriter / JsonParser (#43) own the
// text format, including the depth limit that keeps a hostile `[[[[...` from
// overflowing a recursive-descent parser.
//
// Number notes: integers are stored as int64_t so values above 2^53 survive a
// round trip; doubles are stored separately. isInteger() tells which one a
// value carries, so a serializer can reproduce the original spelling rather
// than printing `42` as `42.0`.
//
// Objects preserve insertion order (a vector of pairs, not a map). Ordering
// costs a linear lookup, but makes encoded output deterministic and therefore
// testable; payloads here are small.
class JsonValue {
public:
  enum class Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };

  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() = default; // null
  JsonValue(const char *value)
      : type_(Type::kString), string_(value != nullptr ? value : "") {}
  JsonValue(std::string value)
      : type_(Type::kString), string_(std::move(value)) {}

  // Named factories for non-strings. Plain constructors would make
  // JsonValue(42) ambiguous (int -> int64_t vs int -> bool), and a payload
  // model is a poor place for a surprising overload set.
  static JsonValue makeBool(bool value) {
    JsonValue result;
    result.type_ = Type::kBool;
    result.bool_ = value;
    return result;
  }
  static JsonValue makeInteger(int64_t value) {
    JsonValue result;
    result.type_ = Type::kNumber;
    result.integer_ = value;
    result.isInteger_ = true;
    return result;
  }
  static JsonValue makeNumber(double value) {
    JsonValue result;
    result.type_ = Type::kNumber;
    result.number_ = value;
    return result;
  }
  static JsonValue makeArray() {
    JsonValue value;
    value.type_ = Type::kArray;
    return value;
  }
  static JsonValue makeObject() {
    JsonValue value;
    value.type_ = Type::kObject;
    return value;
  }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::kNull; }
  bool isBool() const { return type_ == Type::kBool; }
  bool isNumber() const { return type_ == Type::kNumber; }
  bool isString() const { return type_ == Type::kString; }
  bool isArray() const { return type_ == Type::kArray; }
  bool isObject() const { return type_ == Type::kObject; }

  // Accessors do not convert between types: asking a string for asInt() is a
  // caller bug, caught by the assertion in debug builds and by returning the
  // member's default in release builds.
  bool asBool() const {
    assert(isBool());
    return bool_;
  }
  int64_t asInt() const {
    assert(type_ == Type::kNumber && isInteger_);
    return integer_;
  }
  double asNumber() const {
    assert(isNumber());
    return isInteger_ ? static_cast<double>(integer_) : number_;
  }
  bool isInteger() const { return isInteger_; }
  const std::string &asString() const {
    assert(isString());
    return string_;
  }

  Array &asArray() {
    assert(isArray());
    return array_;
  }
  const Array &asArray() const {
    assert(isArray());
    return array_;
  }
  Object &asObject() {
    assert(isObject());
    return object_;
  }
  const Object &asObject() const {
    assert(isObject());
    return object_;
  }

  // Object field lookup. Returns nullptr when absent; does not insert.
  JsonValue *find(std::string_view key) {
    assert(isObject());
    for (auto &entry : object_) {
      if (entry.first == key) {
        return &entry.second;
      }
    }
    return nullptr;
  }
  const JsonValue *find(std::string_view key) const {
    return const_cast<JsonValue *>(this)->find(key);
  }

  // Object field access: inserts a null value when the key is absent, like
  // std::map::operator[]. Use find() when absence must stay observable.
  JsonValue &operator[](const std::string &key) {
    assert(isObject());
    if (JsonValue *existing = find(key)) {
      return *existing;
    }
    object_.emplace_back(key, JsonValue());
    return object_.back().second;
  }

  JsonValue &operator[](size_t index) {
    assert(isArray());
    return array_[index];
  }
  const JsonValue &operator[](size_t index) const {
    assert(isArray());
    return array_[index];
  }

  bool operator==(const JsonValue &other) const {
    if (type_ != other.type_) {
      return false;
    }
    switch (type_) {
    case Type::kNull:
      return true;
    case Type::kBool:
      return bool_ == other.bool_;
    case Type::kNumber:
      if (isInteger_ != other.isInteger_) {
        return false;
      }
      return isInteger_ ? integer_ == other.integer_ : number_ == other.number_;
    case Type::kString:
      return string_ == other.string_;
    case Type::kArray:
      return array_ == other.array_;
    case Type::kObject:
      return object_ == other.object_;
    }
    return false;
  }
  bool operator!=(const JsonValue &other) const { return !(*this == other); }

private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  int64_t integer_ = 0;
  double number_ = 0.0;
  bool isInteger_ = false;
  std::string string_;
  Array array_;
  Object object_;
};

} // namespace reactornet::rpc
