#pragma once

#include <cstdint>
#include <map>
#include <string>

// Simple HTTP 1.0 request parser.
// Only supports GET method.
class HttpRequest {
public:
  enum Method { kInvalid, kGet };

  HttpRequest() : method_(kInvalid), majorVersion_(1), minorVersion_(0) {}

  // Parse the raw HTTP request from the buffer data.
  // 入口总解析函数
  // Returns true if a complete request was parsed.
  bool parseRequest(const char *begin, const char *end);
  // 只读查询接口
  Method method() const { return method_; }
  const std::string &path() const { return path_; }
  const std::map<std::string, std::string> &headers() const { return headers_; }
  const std::string &query() const { return query_; }

  // Get a specific header value. Returns empty string if not found.
  std::string getHeader(const std::string &field) const;

private:
  bool parseRequestLine(const char *begin, const char *end);
  bool parseHeaders(const char *begin, const char *end);
  std::string urlDecode(const std::string &input) const;

  Method method_;                              // 请求方法：kInvalid / kGet
  std::string path_;                           // 解码后的资源路径
  std::string query_;                          // URL查询参数串
  std::map<std::string, std::string> headers_; // 请求头键值对
  int majorVersion_;                           // HTTP主版本，固定1
  int minorVersion_;                           // HTTP次版本，固定0
};
