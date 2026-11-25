#include "Buffer.h"
#include "HttpResponse.h"
#include <gtest/gtest.h>
#include <string>

using namespace reactornet;

namespace {

std::string serialize(const HttpResponse &response) {
  Buffer output;
  response.appendToBuffer(&output);
  return output.retrieveAllAsString();
}

size_t countOccurrences(const std::string &haystack,
                        const std::string &needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

} // namespace

TEST(HttpResponseTest, SerializesStatusLineHeadersAndBody) {
  HttpResponse response(false);
  response.setStatusCode(HttpResponse::k200Ok);
  response.setContentType("text/plain");
  response.setBody("abc");

  const std::string output = serialize(response);
  EXPECT_EQ(output.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
  EXPECT_NE(output.find("Content-Length: 3\r\n"), std::string::npos);
  EXPECT_NE(output.find("Content-Type: text/plain\r\n"), std::string::npos);
  EXPECT_NE(output.find("\r\n\r\nabc"), std::string::npos);
  EXPECT_EQ(output.find("Connection: close"), std::string::npos);
}

TEST(HttpResponseTest, HeadSuppressionOmitsBodyButKeepsContentLength) {
  HttpResponse response(false);
  response.setBody("hello");
  response.setSuppressBody(true);

  const std::string output = serialize(response);
  EXPECT_NE(output.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_EQ(output.size(), output.find("\r\n\r\n") + 4);
}

TEST(HttpResponseTest, NewStatusLinesSerialize) {
  struct Case {
    HttpResponse::HttpStatusCode code;
    const char *line;
  };
  const Case cases[] = {
      {HttpResponse::k405MethodNotAllowed,
       "HTTP/1.1 405 Method Not Allowed\r\n"},
      {HttpResponse::k413PayloadTooLarge,
       "HTTP/1.1 413 Payload Too Large\r\n"},
      {HttpResponse::k501NotImplemented,
       "HTTP/1.1 501 Not Implemented\r\n"},
      {HttpResponse::k505HttpVersionNotSupported,
       "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
  };

  for (const Case &testCase : cases) {
    HttpResponse response(true);
    response.setStatusCode(testCase.code);
    const std::string output = serialize(response);
    EXPECT_EQ(output.rfind(testCase.line, 0), 0u) << testCase.line;
  }
}

TEST(HttpResponseTest, CustomContentLengthCannotOverrideDerivedValue) {
  HttpResponse response(false);
  response.setBody("abc");
  response.addHeader("Content-Length", "999");
  response.addHeader("content-length", "888");

  const std::string output = serialize(response);
  // Both spellings contain "ontent-Length"; exactly one line must survive.
  EXPECT_EQ(countOccurrences(output, "ontent-Length"), 1u);
  EXPECT_NE(output.find("Content-Length: 3\r\n"), std::string::npos);
}

TEST(HttpResponseTest, AllowHeaderIsSerialized) {
  HttpResponse response(false);
  response.setStatusCode(HttpResponse::k405MethodNotAllowed);
  response.addHeader("Allow", "GET, HEAD");

  const std::string output = serialize(response);
  EXPECT_NE(output.find("Allow: GET, HEAD\r\n"), std::string::npos);
}

TEST(HttpResponseTest, CloseConnectionHeaderOnlyWhenRequested) {
  HttpResponse keepAlive(false);
  EXPECT_EQ(serialize(keepAlive).find("Connection: close"),
            std::string::npos);

  HttpResponse closing(true);
  closing.addHeader("Connection", "keep-alive"); // must not duplicate/override
  const std::string output = serialize(closing);
  EXPECT_EQ(countOccurrences(output, "Connection:"), 1u);
  EXPECT_NE(output.find("Connection: close\r\n"), std::string::npos);
}

TEST(HttpResponseTest, BinaryBodyRoundTrips) {
  const std::string body("a\0b", 3);
  HttpResponse response(false);
  response.setBody(body);

  const std::string output = serialize(response);
  EXPECT_NE(output.find("Content-Length: 3\r\n"), std::string::npos);
  EXPECT_EQ(output.substr(output.find("\r\n\r\n") + 4), body);
}

TEST(HttpResponseTest, SuppressBodyStillEmitsHeadersForErrors) {
  const std::string body = "<html>not found</html>";
  HttpResponse response(true);
  response.setStatusCode(HttpResponse::k404NotFound);
  response.setContentType("text/html");
  response.setBody(body);
  response.setSuppressBody(true);

  const std::string output = serialize(response);
  EXPECT_EQ(output.rfind("HTTP/1.1 404 Not Found\r\n", 0), 0u);
  EXPECT_NE(output.find("Content-Length: " + std::to_string(body.size()) +
                        "\r\n"),
            std::string::npos);
  EXPECT_EQ(output.size(), output.find("\r\n\r\n") + 4);
}
