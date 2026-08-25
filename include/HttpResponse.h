#pragma once

#include <map>
#include <string>

class Buffer;

// Builds an HTTP 1.0 response.
class HttpResponse {
public:
  enum HttpStatusCode {
    k200Ok = 200,
    k400BadRequest = 400,
    k404NotFound = 404,
    k500InternalServerError = 500
  };

  HttpResponse(bool close = true)
      : statusCode_(k200Ok), closeConnection_(close) {}
  // 状态设置
  void setStatusCode(HttpStatusCode code) { statusCode_ = code; }
  void setStatusMessage(const std::string &message) {
    statusMessage_ = message;
  }
  void setCloseConnection(bool close) { closeConnection_ = close; }
  // 头部操作
  void setContentType(const std::string &contentType) {
    addHeader("Content-Type", contentType);
  }

  void addHeader(const std::string &key, const std::string &value) {
    headers_[key] = value;
  }

  void setBody(const std::string &body) { body_ = body; }

  // Serialize the response to a Buffer for sending.
  // 序列化接口
  void appendToBuffer(Buffer *output) const;

private:
  std::string statusMessageForCode(HttpStatusCode code) const;

  HttpStatusCode statusCode_;                  // 响应状态码
  std::string statusMessage_;                  // 状态描述文字
  std::map<std::string, std::string> headers_; // 全部响应头键值对
  std::string body_;                           // 响应正文
  bool closeConnection_; // 是否短连接（Connection: close）
};
