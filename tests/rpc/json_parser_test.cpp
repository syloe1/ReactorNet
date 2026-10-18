#include <gtest/gtest.h>

#include "rpc/json_parser.h"
#include "rpc/json_serializer.h"
#include "rpc/json_value.h"
#include "rpc/json_writer.h"
#include "rpc/protocol.h"

#include <cstdint>
#include <limits>
#include <random>
#include <string>

using namespace reactornet::rpc;

namespace {

std::string nestedArrays(size_t levels) {
  return std::string(levels, '[') + std::string(levels, ']');
}

TEST(JsonParserTest, ParsesScalars) {
  JsonParser parser;
  JsonValue value;

  ASSERT_TRUE(parser.parse("null", &value));
  EXPECT_TRUE(value.isNull());

  ASSERT_TRUE(parser.parse(" true ", &value));
  ASSERT_TRUE(value.isBool());
  EXPECT_TRUE(value.asBool());

  ASSERT_TRUE(parser.parse("false", &value));
  ASSERT_TRUE(value.isBool());
  EXPECT_FALSE(value.asBool());

  ASSERT_TRUE(parser.parse("0", &value));
  ASSERT_TRUE(value.isInteger());
  EXPECT_EQ(value.asInt(), 0);

  ASSERT_TRUE(parser.parse("-42", &value));
  ASSERT_TRUE(value.isInteger());
  EXPECT_EQ(value.asInt(), -42);

  ASSERT_TRUE(parser.parse("9223372036854775807", &value));
  ASSERT_TRUE(value.isInteger());
  EXPECT_EQ(value.asInt(), std::numeric_limits<int64_t>::max());

  ASSERT_TRUE(parser.parse("-9223372036854775808", &value));
  ASSERT_TRUE(value.isInteger());
  EXPECT_EQ(value.asInt(), std::numeric_limits<int64_t>::min());

  ASSERT_TRUE(parser.parse("\"hi\"", &value));
  ASSERT_TRUE(value.isString());
  EXPECT_EQ(value.asString(), "hi");
}

TEST(JsonParserTest, ParsesFloatingPointNumbers) {
  JsonParser parser;
  JsonValue value;

  ASSERT_TRUE(parser.parse("1.5", &value));
  EXPECT_FALSE(value.isInteger());
  EXPECT_DOUBLE_EQ(value.asNumber(), 1.5);

  ASSERT_TRUE(parser.parse("-0.25", &value));
  EXPECT_DOUBLE_EQ(value.asNumber(), -0.25);

  ASSERT_TRUE(parser.parse("1e3", &value));
  EXPECT_FALSE(value.isInteger());
  EXPECT_DOUBLE_EQ(value.asNumber(), 1000.0);

  ASSERT_TRUE(parser.parse("1E+2", &value));
  EXPECT_DOUBLE_EQ(value.asNumber(), 100.0);

  ASSERT_TRUE(parser.parse("1e-3", &value));
  EXPECT_DOUBLE_EQ(value.asNumber(), 0.001);

  // Valid JSON, but outside int64: the object model's only lossless option
  // is a double, so it stays a number rather than being rejected.
  ASSERT_TRUE(parser.parse("9223372036854775808", &value));
  EXPECT_FALSE(value.isInteger());
  EXPECT_DOUBLE_EQ(value.asNumber(), 9223372036854775808.0);
}

TEST(JsonParserTest, RejectsMalformedNumbers) {
  JsonParser parser;
  const char *bad[] = {"01",  "+1",       ".5",     "1.",
                       "1e", "1e+",       "--1",    "0x10",
                       "1e999", "-1e999", "NaN",    "Infinity"};
  for (const char *text : bad) {
    JsonValue value;
    EXPECT_FALSE(parser.parse(text, &value)) << text;
  }
}

TEST(JsonParserTest, DecodesEscapes) {
  JsonParser parser;
  JsonValue value;

  ASSERT_TRUE(parser.parse("\"a\\\"b\\\\c\\/d\"", &value));
  EXPECT_EQ(value.asString(), "a\"b\\c/d");

  ASSERT_TRUE(parser.parse("\"a\\nb\\tc\\bd\\fe\\rf\"", &value));
  EXPECT_EQ(value.asString(), "a\nb\tc\bd\fe\rf");

  ASSERT_TRUE(parser.parse("\"\\u0041\\u4F60\\u597D\"", &value));
  EXPECT_EQ(value.asString(), "A\xE4\xBD\xA0\xE5\xA5\xBD");

  // Surrogate pair for U+1F600.
  ASSERT_TRUE(parser.parse("\"\\uD83D\\uDE00\"", &value));
  EXPECT_EQ(value.asString(), "\xF0\x9F\x98\x80");

  // \u0000 is legal and must survive as an embedded NUL.
  ASSERT_TRUE(parser.parse("\"a\\u0000b\"", &value));
  ASSERT_EQ(value.asString().size(), 3u);
  EXPECT_EQ(value.asString()[1], '\0');
}

TEST(JsonParserTest, RejectsMalformedStrings) {
  JsonParser parser;
  const char *bad[] = {
      "\"unterminated",
      "\"bad\\qescape\"",
      "\"\\u12\"",
      "\"\\u12G4\"",
      "\"\\uD800\"",           // lone high surrogate
      "\"\\uDC00\"",           // lone low surrogate
      "\"\\uD800x\"",          // high surrogate without its pair
      "\"\\uD800\\u0041\"",    // high surrogate followed by non-low
      "\"raw\ncontrol\"",      // literal control character
      "\"\x80\"",              // invalid raw UTF-8
      "\"\xC0\xAF\"",          // overlong
      "\"\xED\xA0\x80\"",      // raw surrogate
  };
  for (const char *text : bad) {
    JsonValue value;
    EXPECT_FALSE(parser.parse(text, &value)) << text;
  }
}

TEST(JsonParserTest, ParsesArraysAndObjects) {
  JsonParser parser;
  JsonValue value;

  ASSERT_TRUE(parser.parse("[]", &value));
  ASSERT_TRUE(value.isArray());
  EXPECT_TRUE(value.asArray().empty());

  ASSERT_TRUE(parser.parse("{}", &value));
  ASSERT_TRUE(value.isObject());
  EXPECT_TRUE(value.asObject().empty());

  ASSERT_TRUE(parser.parse(" \t\r\n [ 1, \"two\", true, null ] \n", &value));
  ASSERT_TRUE(value.isArray());
  ASSERT_EQ(value.asArray().size(), 4u);
  EXPECT_EQ(value[0].asInt(), 1);
  EXPECT_EQ(value[1].asString(), "two");
  EXPECT_TRUE(value[2].asBool());
  EXPECT_TRUE(value[3].isNull());

  ASSERT_TRUE(parser.parse("{\"b\":1,\"a\":[2,{\"c\":3}]}", &value));
  ASSERT_TRUE(value.isObject());
  ASSERT_EQ(value.asObject().size(), 2u);
  EXPECT_EQ(value.asObject()[0].first, "b");
  EXPECT_EQ(value.asObject()[1].first, "a");
  EXPECT_EQ(value.find("b")->asInt(), 1);
  ASSERT_NE(value.find("a"), nullptr);
  EXPECT_EQ((*value.find("a"))[0].asInt(), 2);
  EXPECT_EQ((*value.find("a"))[1].find("c")->asInt(), 3);

  // Duplicate names are syntactically valid JSON; keeping both preserves the
  // input rather than silently discarding one.
  ASSERT_TRUE(parser.parse("{\"a\":1,\"a\":2}", &value));
  ASSERT_EQ(value.asObject().size(), 2u);
}

TEST(JsonParserTest, RejectsMalformedStructures) {
  JsonParser parser;
  const char *bad[] = {"[",       "[1",      "[1,",     "[1,]",
                       "[,]",     "[1 2]",   "{",       "{\"a\"",
                       "{\"a\":", "{\"a\" 1}", "{\"a\":1,",
                       "{\"a\":1,}", "{1:2}", "{\"a\":1 \"b\":2}",
                       "[]]",     "null x",  "{} {}",   "",
                       "   "};
  for (const char *text : bad) {
    JsonValue value;
    EXPECT_FALSE(parser.parse(text, &value)) << "accepted: " << text;
  }
}

TEST(JsonParserTest, EnforcesTheDepthLimitBeforeRecursing) {
  JsonParser parser;
  JsonValue value;

  EXPECT_TRUE(parser.parse(nestedArrays(JsonParser::kDefaultMaxDepth), &value));
  EXPECT_FALSE(
      parser.parse(nestedArrays(JsonParser::kDefaultMaxDepth + 1), &value));

  JsonParser shallow(1);
  EXPECT_TRUE(shallow.parse("[1]", &value));
  EXPECT_FALSE(shallow.parse("[[1]]", &value));

  // A structural bomb must fail rather than overflow the stack.
  EXPECT_FALSE(parser.parse(nestedArrays(100000), &value));
}

TEST(JsonParserTest, LeavesTheOutputUntouchedOnFailure) {
  JsonParser parser;
  JsonValue value = JsonValue::makeInteger(7);
  EXPECT_FALSE(parser.parse("not json", &value));
  EXPECT_EQ(value, JsonValue::makeInteger(7));
  EXPECT_FALSE(parser.parse("null", nullptr));
}

TEST(JsonParserTest, WriterOutputRoundTrips) {
  JsonValue original = JsonValue::makeObject();
  original["null"] = JsonValue();
  original["bool"] = JsonValue::makeBool(true);
  original["int"] = JsonValue::makeInteger(9007199254740993LL);
  original["double"] = JsonValue::makeNumber(1.0);
  original["string"] = JsonValue("quote \" slash \\ newline \n");
  original["unicode"] = JsonValue("你好 \xF0\x9F\x99\x82");

  JsonValue array = JsonValue::makeArray();
  array.asArray().push_back(JsonValue::makeInteger(1));
  array.asArray().push_back(original["string"]);
  array.asArray().push_back(JsonValue::makeObject());
  original["array"] = array;

  const std::string text = JsonWriter().write(original);
  ASSERT_FALSE(text.empty());
  JsonValue parsed;
  ASSERT_TRUE(JsonParser().parse(text, &parsed)) << text;
  EXPECT_EQ(parsed, original);
}

TEST(JsonParserTest, RandomBytesNeverCrash) {
  // Fixed seed: a failure is reproducible. The assertion is only that parsing
  // arbitrary bytes is memory-safe; ASan/UBSan make that observable.
  std::mt19937 rng(0x5EED);
  std::uniform_int_distribution<size_t> length(0, 64);
  std::uniform_int_distribution<int> byte(0, 255);

  for (int i = 0; i < 2000; ++i) {
    std::string input(length(rng), '\0');
    for (char &c : input) {
      c = static_cast<char>(byte(rng));
    }
    JsonValue value;
    JsonParser().parse(input, &value);
  }
}

TEST(JsonSerializerTest, NameAndIdMatchTheWireConstant) {
  JsonSerializer serializer;
  EXPECT_EQ(serializer.name(), "json");
  EXPECT_EQ(serializer.id(), kSerializerJson);
}

TEST(JsonSerializerTest, RoundTripsAValue) {
  JsonSerializer serializer;

  JsonValue value = JsonValue::makeObject();
  value["message"] = JsonValue("hello");
  value["n"] = JsonValue::makeInteger(7);
  value["list"] = JsonValue::makeArray();
  value["list"].asArray().push_back(JsonValue::makeNumber(1.5));

  std::string encoded;
  ASSERT_TRUE(serializer.encode(value, &encoded));
  EXPECT_EQ(encoded, "{\"message\":\"hello\",\"n\":7,\"list\":[1.5]}");

  JsonValue decoded;
  ASSERT_TRUE(serializer.decode(encoded, &decoded));
  EXPECT_EQ(decoded, value);
}

TEST(JsonSerializerTest, RejectsMalformedInputAndLeavesTheOutput) {
  JsonSerializer serializer;
  JsonValue decoded = JsonValue::makeInteger(7);
  EXPECT_FALSE(serializer.decode("{not json}", &decoded));
  EXPECT_EQ(decoded, JsonValue::makeInteger(7));

  std::string out = "sentinel";
  EXPECT_FALSE(serializer.encode(
      JsonValue::makeNumber(std::numeric_limits<double>::infinity()), &out));
  EXPECT_EQ(out, "sentinel");
}

TEST(JsonSerializerTest, AppliesTheDepthLimitToBothDirections) {
  JsonSerializer shallow(2);
  JsonValue value;
  EXPECT_FALSE(shallow.decode(nestedArrays(3), &value));
  EXPECT_TRUE(shallow.decode(nestedArrays(2), &value));

  std::string out;
  EXPECT_TRUE(shallow.encode(value, &out));

  JsonValue tooDeep;
  ASSERT_TRUE(JsonParser().parse(nestedArrays(3), &tooDeep));
  out = "sentinel";
  EXPECT_FALSE(shallow.encode(tooDeep, &out));
  EXPECT_EQ(out, "sentinel");
}

} // namespace
