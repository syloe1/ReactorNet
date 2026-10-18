#include <gtest/gtest.h>

#include "rpc/json_value.h"
#include "rpc/json_writer.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

using namespace reactornet::rpc;

namespace {

TEST(JsonWriterTest, WritesScalars) {
  const JsonWriter writer;
  EXPECT_EQ(writer.write(JsonValue()), "null");
  EXPECT_EQ(writer.write(JsonValue::makeBool(true)), "true");
  EXPECT_EQ(writer.write(JsonValue::makeBool(false)), "false");
  EXPECT_EQ(writer.write(JsonValue::makeInteger(0)), "0");
  EXPECT_EQ(writer.write(JsonValue::makeInteger(-42)), "-42");
  EXPECT_EQ(writer.write(JsonValue::makeInteger(
                std::numeric_limits<int64_t>::min())),
            "-9223372036854775808");
  // Above 2^53: only an integer member survives this, a double would round.
  EXPECT_EQ(writer.write(JsonValue::makeInteger(9007199254740993LL)),
            "9007199254740993");
}

TEST(JsonWriterTest, DoublesStayDoubles) {
  const JsonWriter writer;
  EXPECT_EQ(writer.write(JsonValue::makeNumber(1.5)), "1.5");
  EXPECT_EQ(writer.write(JsonValue::makeNumber(0.1)), "0.1");
  // Without the suffix this would parse back as an integer JsonValue.
  EXPECT_EQ(writer.write(JsonValue::makeNumber(1.0)), "1.0");
  EXPECT_EQ(writer.write(JsonValue::makeNumber(-0.0)), "-0.0");
}

TEST(JsonWriterTest, ExtremeDoublesRoundTripThroughStrtod) {
  const JsonWriter writer;
  const double values[] = {1e300, 5e-324,
                           std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::min()};
  for (const double value : values) {
    const std::string text = writer.write(JsonValue::makeNumber(value));
    ASSERT_FALSE(text.empty());
    char *end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(*end, '\0') << text;
    EXPECT_EQ(parsed, value) << text;
  }
}

TEST(JsonWriterTest, RejectsNonFiniteNumbers) {
  const JsonWriter writer;
  std::string out = "sentinel";
  EXPECT_FALSE(writer.write(
      JsonValue::makeNumber(std::numeric_limits<double>::quiet_NaN()), &out));
  EXPECT_EQ(out, "sentinel");
  EXPECT_FALSE(writer.write(
      JsonValue::makeNumber(std::numeric_limits<double>::infinity()), &out));
  EXPECT_EQ(out, "sentinel");
  EXPECT_FALSE(writer.write(
      JsonValue::makeNumber(-std::numeric_limits<double>::infinity()), &out));
  EXPECT_EQ(out, "sentinel");
  EXPECT_EQ(writer.write(
                JsonValue::makeNumber(std::numeric_limits<double>::quiet_NaN())),
            "");
}

TEST(JsonWriterTest, EscapesQuotesBackslashesAndControls) {
  const JsonWriter writer;
  EXPECT_EQ(writer.write(JsonValue("")), "\"\"");
  EXPECT_EQ(writer.write(JsonValue("hello")), "\"hello\"");
  EXPECT_EQ(writer.write(JsonValue("a\"b")), "\"a\\\"b\"");
  EXPECT_EQ(writer.write(JsonValue("a\\b")), "\"a\\\\b\"");
  // Solidus is legal unescaped and escaping it would only bloat payloads.
  EXPECT_EQ(writer.write(JsonValue("a/b")), "\"a/b\"");

  const std::string controls("a\nb\tc\bd\fe\rf\x01", 12);
  EXPECT_EQ(writer.write(JsonValue(controls)),
            "\"a\\nb\\tc\\bd\\fe\\rf\\u0001\"");
}

TEST(JsonWriterTest, PassesValidUtf8ThroughUnchanged) {
  const JsonWriter writer;
  EXPECT_EQ(writer.write(JsonValue("hello 你好 \xF0\x9F\x99\x82")),
            "\"hello 你好 \xF0\x9F\x99\x82\"");
}

TEST(JsonWriterTest, RejectsInvalidUtf8WithoutTouchingTheOutput) {
  const JsonWriter writer;
  const std::string invalid[] = {
      std::string("\x80", 1),               // stray continuation
      std::string("\xC0\xAF", 2),           // overlong '/'
      std::string("\xE0\x80\xAF", 3),       // overlong 3-byte
      std::string("\xED\xA0\x80", 3),       // UTF-16 surrogate
      std::string("\xF4\x90\x80\x80", 4),   // above U+10FFFF
      std::string("\xE2\x82", 2),           // truncated
      std::string("ok\xFF", 3),             // invalid lead after valid text
  };
  for (const std::string &text : invalid) {
    std::string out = "sentinel";
    EXPECT_FALSE(writer.write(JsonValue(text), &out))
        << "accepted invalid UTF-8: " << text;
    EXPECT_EQ(out, "sentinel");
  }
}

TEST(JsonWriterTest, WritesArraysAndObjectsInInsertionOrder) {
  const JsonWriter writer;

  EXPECT_EQ(writer.write(JsonValue::makeArray()), "[]");
  EXPECT_EQ(writer.write(JsonValue::makeObject()), "{}");

  JsonValue array = JsonValue::makeArray();
  array.asArray().push_back(JsonValue::makeInteger(1));
  array.asArray().push_back(JsonValue("two"));
  array.asArray().push_back(JsonValue::makeBool(true));
  array.asArray().push_back(JsonValue());
  EXPECT_EQ(writer.write(array), "[1,\"two\",true,null]");

  JsonValue object = JsonValue::makeObject();
  object["b"] = JsonValue::makeInteger(1);
  object["a"] = JsonValue::makeInteger(2);
  object["nested"] = array;
  EXPECT_EQ(writer.write(object), "{\"b\":1,\"a\":2,\"nested\":[1,\"two\",true,null]}");
}

TEST(JsonWriterTest, EnforcesTheDefaultDepthLimit) {
  const auto nestedArrays = [](size_t levels) {
    JsonValue root = JsonValue::makeArray();
    JsonValue *cursor = &root;
    for (size_t i = 1; i < levels; ++i) {
      cursor->asArray().push_back(JsonValue::makeArray());
      cursor = &cursor->asArray().back();
    }
    return root;
  };

  const JsonWriter writer;
  std::string out;
  EXPECT_TRUE(writer.write(nestedArrays(JsonWriter::kDefaultMaxDepth), &out));
  EXPECT_EQ(out.front(), '[');
  EXPECT_EQ(out.back(), ']');

  out = "sentinel";
  EXPECT_FALSE(
      writer.write(nestedArrays(JsonWriter::kDefaultMaxDepth + 1), &out));
  EXPECT_EQ(out, "sentinel");
}

TEST(JsonWriterTest, CustomDepthLimitIsCountedPerContainer) {
  JsonValue root = JsonValue::makeArray();
  root.asArray().push_back(JsonValue::makeInteger(1));

  const JsonWriter oneLevel(1);
  std::string out;
  EXPECT_TRUE(oneLevel.write(root, &out));
  EXPECT_EQ(out, "[1]");

  root.asArray().push_back(JsonValue::makeArray());
  out = "sentinel";
  EXPECT_FALSE(oneLevel.write(root, &out));
  EXPECT_EQ(out, "sentinel");
}

TEST(JsonWriterTest, NullOutputPointerIsRejected) {
  const JsonWriter writer;
  EXPECT_FALSE(writer.write(JsonValue(), nullptr));
}

} // namespace
