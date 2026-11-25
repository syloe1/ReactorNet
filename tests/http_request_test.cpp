#include "HttpRequest.h"
#include <gtest/gtest.h>
#include <string>

using namespace reactornet;

namespace {

struct ParseOutcome {
  HttpRequest::ParseResult result;
  size_t consumed;
};

// A sentinel initial value proves parse() always initializes *consumed.
ParseOutcome parseRequest(const std::string &raw, HttpRequest *request) {
  size_t consumed = 12345;
  const HttpRequest::ParseResult result =
      request->parse(raw.data(), raw.data() + raw.size(), &consumed);
  return {result, consumed};
}

} // namespace

TEST(HttpRequestTest, ParsesGetRequestAndReportsConsumed) {
  const std::string raw =
      "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(raw, &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.method(), HttpRequest::Method::kGet);
  EXPECT_EQ(request.path(), "/index.html");
  EXPECT_EQ(request.majorVersion(), 1);
  EXPECT_EQ(request.minorVersion(), 1);
  EXPECT_EQ(request.getHeader("host"), "example.com");
  EXPECT_EQ(request.body(), "");
  EXPECT_EQ(outcome.consumed, raw.size());
}

TEST(HttpRequestTest, ParsesHeadAndPostMethods) {
  HttpRequest head;
  EXPECT_EQ(parseRequest("HEAD /x HTTP/1.1\r\n\r\n", &head).result,
            HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(head.method(), HttpRequest::Method::kHead);

  HttpRequest post;
  const std::string raw =
      "POST /echo HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
  EXPECT_EQ(parseRequest(raw, &post).result,
            HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(post.method(), HttpRequest::Method::kPost);
  EXPECT_EQ(post.body(), "");
}

TEST(HttpRequestTest, UnknownMethodIsNotImplemented) {
  HttpRequest request;
  const ParseOutcome outcome =
      parseRequest("PUT / HTTP/1.1\r\n\r\n", &request);

  EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError);
  EXPECT_EQ(request.error(), HttpRequest::ParseError::kNotImplemented);
  EXPECT_EQ(outcome.consumed, 0u);
}

TEST(HttpRequestTest, MalformedRequestLineIsBadRequest) {
  const char *bad[] = {
      "GET /\r\n\r\n",                       // missing version
      "GET / HTTP/1.1 extra\r\n\r\n",        // fourth token
      "GET http://example.com/ HTTP/1.1\r\n\r\n", // absolute URI target
      "GET\r\n\r\n",                         // only one token
  };

  for (const char *raw : bad) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << raw;
    EXPECT_EQ(request.error(), HttpRequest::ParseError::kBadRequest) << raw;
  }
}

TEST(HttpRequestTest, VersionParsingAndPolicy) {
  for (const char *raw : {"GET / HTTP/1.0\r\n\r\n", "GET / HTTP/1.1\r\n\r\n"}) {
    HttpRequest request;
    EXPECT_EQ(parseRequest(raw, &request).result,
              HttpRequest::ParseResult::kComplete)
        << raw;
  }

  for (const char *raw :
       {"GET / HTTP/1.2\r\n\r\n", "GET / HTTP/2.0\r\n\r\n",
        "GET / HTTP/0.9\r\n\r\n"}) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << raw;
    EXPECT_EQ(request.error(),
              HttpRequest::ParseError::kHttpVersionNotSupported)
        << raw;
  }

  for (const char *raw :
       {"GET / HTTP/1\r\n\r\n", "GET / HTTP/x.1\r\n\r\n",
        "GET / HTTP/1.\r\n\r\n", "GET / HTTP/1.1.1\r\n\r\n"}) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << raw;
    EXPECT_EQ(request.error(), HttpRequest::ParseError::kBadRequest) << raw;
  }
}

TEST(HttpRequestTest, IncompleteRequestLineNeedsMoreData) {
  for (const char *raw : {"", "GET", "GET / HTTP/1.1"}) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kNeedMoreData) << raw;
    EXPECT_EQ(outcome.consumed, 0u) << raw;
  }
}

TEST(HttpRequestTest, IncompleteHeadersNeedMoreData) {
  HttpRequest request;
  const ParseOutcome outcome =
      parseRequest("GET / HTTP/1.1\r\nHost: x\r\n", &request);

  EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kNeedMoreData);
  EXPECT_EQ(outcome.consumed, 0u);
}

TEST(HttpRequestTest, BodySplitAcrossCallsNeedsMoreData) {
  const std::string prefix =
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel";
  HttpRequest request;
  const ParseOutcome partial = parseRequest(prefix, &request);

  EXPECT_EQ(partial.result, HttpRequest::ParseResult::kNeedMoreData);
  EXPECT_EQ(partial.consumed, 0u);

  const std::string full =
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  const ParseOutcome complete = parseRequest(full, &request);
  EXPECT_EQ(complete.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.body(), "hello");
}

TEST(HttpRequestTest, CompleteBodyIsCopiedAndConsumed) {
  const std::string raw =
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(raw, &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.body(), "hello");
  EXPECT_EQ(outcome.consumed, raw.size());
}

TEST(HttpRequestTest, ZeroContentLengthCompletesAtHeaderEnd) {
  const std::string raw =
      "POST /echo HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(raw, &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.body(), "");
  EXPECT_EQ(outcome.consumed, raw.size());
}

TEST(HttpRequestTest, PostWithoutContentLengthHasEmptyBody) {
  const std::string raw = "POST /echo HTTP/1.1\r\n\r\n";
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(raw, &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.body(), "");
  EXPECT_EQ(outcome.consumed, raw.size());
}

TEST(HttpRequestTest, ConsumedStopsBeforePipelinedRequest) {
  const std::string first =
      "POST /echo HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi";
  const std::string second = "GET /next HTTP/1.1\r\n\r\n";
  const std::string all = first + second;

  HttpRequest request;
  const ParseOutcome outcome = parseRequest(all, &request);
  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(outcome.consumed, first.size());
  EXPECT_EQ(request.body(), "hi");

  HttpRequest next;
  size_t nextConsumed = 0;
  ASSERT_EQ(next.parse(all.data() + outcome.consumed, all.data() + all.size(),
                       &nextConsumed),
            HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(next.path(), "/next");
  EXPECT_EQ(nextConsumed, second.size());
}

TEST(HttpRequestTest, ContentLengthValidation) {
  for (const char *value : {"abc", "+5", "", "5 5"}) {
    const std::string raw =
        std::string("POST /echo HTTP/1.1\r\nContent-Length: ") + value +
        "\r\n\r\n";
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << value;
    EXPECT_EQ(request.error(), HttpRequest::ParseError::kBadRequest) << value;
  }
}

TEST(HttpRequestTest, OversizedContentLengthIsRejectedBeforeBody) {
  for (const char *value :
       {"1048577", "999999999999999999999999999999"}) {
    const std::string raw =
        std::string("POST /echo HTTP/1.1\r\nContent-Length: ") + value +
        "\r\n\r\n";
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << value;
    EXPECT_EQ(request.error(),
              HttpRequest::ParseError::kPayloadTooLarge)
        << value;
  }

  const std::string body(HttpRequest::kMaxBodySize, 'x');
  const std::string raw =
      "POST /echo HTTP/1.1\r\nContent-Length: 1048576\r\n\r\n" + body;
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(raw, &request);
  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.body().size(), HttpRequest::kMaxBodySize);
}

TEST(HttpRequestTest, DuplicateContentLengthRules) {
  HttpRequest same;
  const std::string sameRaw =
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n"
      "content-length: 5\r\n\r\nhello";
  const ParseOutcome sameOutcome = parseRequest(sameRaw, &same);
  ASSERT_EQ(sameOutcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(same.body(), "hello");

  HttpRequest conflicting;
  const std::string conflictingRaw =
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n"
      "Content-Length: 6\r\n\r\nhello";
  const ParseOutcome conflictingOutcome = parseRequest(conflictingRaw, &conflicting);
  EXPECT_EQ(conflictingOutcome.result, HttpRequest::ParseResult::kError);
  EXPECT_EQ(conflicting.error(), HttpRequest::ParseError::kBadRequest);
}

TEST(HttpRequestTest, HeaderLookupIsCaseInsensitive) {
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(
      "GET / HTTP/1.1\r\nhOsT: example.com\r\n\r\n", &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.getHeader("Host"), "example.com");
  EXPECT_EQ(request.getHeader("HOST"), "example.com");
  EXPECT_EQ(request.getHeader("host"), "example.com");
  EXPECT_EQ(request.headers().size(), 1u);
}

TEST(HttpRequestTest, MalformedHeaderLineIsBadRequest) {
  const char *bad[] = {
      "GET / HTTP/1.1\r\nBadHeader\r\n\r\n",              // no colon
      "GET / HTTP/1.1\r\nContent-Length : 5\r\n\r\n",     // space in name
      "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n",     // obs-fold
      "GET / HTTP/1.1\r\n: value\r\n\r\n",                // empty name
  };

  for (const char *raw : bad) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << raw;
    EXPECT_EQ(request.error(), HttpRequest::ParseError::kBadRequest) << raw;
  }
}

TEST(HttpRequestTest, TransferEncodingIsNotImplemented) {
  HttpRequest request;
  const ParseOutcome outcome = parseRequest(
      "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
      &request);

  EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError);
  EXPECT_EQ(request.error(), HttpRequest::ParseError::kNotImplemented);
}

TEST(HttpRequestTest, QueryIsSplitAndLeftRaw) {
  HttpRequest request;
  const ParseOutcome outcome =
      parseRequest("GET /a/b?x=1&y=2 HTTP/1.1\r\n\r\n", &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.path(), "/a/b");
  EXPECT_EQ(request.query(), "x=1&y=2");
}

TEST(HttpRequestTest, PathIsPercentDecoded) {
  HttpRequest index;
  ASSERT_EQ(parseRequest("GET /%69ndex.html HTTP/1.1\r\n\r\n", &index).result,
            HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(index.path(), "/index.html");

  HttpRequest slash;
  ASSERT_EQ(parseRequest("GET /a%2fb HTTP/1.1\r\n\r\n", &slash).result,
            HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(slash.path(), "/a/b");
}

TEST(HttpRequestTest, TraversalIsRejectedAfterDecoding) {
  for (const char *raw :
       {"GET /%2e%2e/etc/passwd HTTP/1.1\r\n\r\n",
        "GET /a/%2e%2e/b HTTP/1.1\r\n\r\n",
        "GET /a%00b HTTP/1.1\r\n\r\n"}) {
    HttpRequest request;
    const ParseOutcome outcome = parseRequest(raw, &request);
    EXPECT_EQ(outcome.result, HttpRequest::ParseResult::kError) << raw;
    EXPECT_EQ(request.error(), HttpRequest::ParseError::kBadRequest) << raw;
  }
}

TEST(HttpRequestTest, RootPathMapsToIndexHtml) {
  HttpRequest request;
  const ParseOutcome outcome =
      parseRequest("GET /?q=1 HTTP/1.1\r\n\r\n", &request);

  ASSERT_EQ(outcome.result, HttpRequest::ParseResult::kComplete);
  EXPECT_EQ(request.path(), "/index.html");
  EXPECT_EQ(request.query(), "q=1");
}
