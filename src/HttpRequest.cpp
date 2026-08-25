#include "HttpRequest.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

// Simple URL decode: %XX -> character
std::string HttpRequest::urlDecode(const std::string &input) const {
  std::string result;
  result.reserve(input.size());

  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int high = input[i + 1];
      int low = input[i + 2];

      auto hexToInt = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };

      int h = hexToInt(static_cast<char>(high));
      int l = hexToInt(static_cast<char>(low));
      if (h >= 0 && l >= 0) {
        result.push_back(static_cast<char>((h << 4) | l));
        i += 2;
      } else {
        result.push_back('%');
      }
    } else if (input[i] == '+') {
      result.push_back(' ');
    } else {
      result.push_back(input[i]);
    }
  }
  return result;
}

bool HttpRequest::parseRequest(const char *begin, const char *end) {
  // 在 [begin, end) 缓冲区查找 "\r\n"
  const char *crlf = std::search(begin, end, "\r\n", "\r\n" + 2);
  if (crlf == end) {
    return false; // No complete request line yet
  }

  // Parse request line
  if (!parseRequestLine(begin, crlf)) {
    return false;
  }

  // Parse headers
  const char *headersStart = crlf + 2;
  if (!parseHeaders(headersStart, end)) {
    return false;
  }

  return true;
}

bool HttpRequest::parseRequestLine(const char *begin, const char *end) {
  std::string requestLine(begin, end - begin);

  // Expected format: METHOD PATH HTTP/1.X
  std::istringstream iss(requestLine);
  std::string method, path, version;
  if (!(iss >> method >> path >> version)) {
    std::cerr << "[HttpRequest] Malformed request line: " << requestLine
              << std::endl;
    return false;
  }

  // Parse method
  if (method == "GET") {
    method_ = kGet;
  } else {
    std::cerr << "[HttpRequest] Unsupported method: " << method << std::endl;
    method_ = kInvalid;
    return false;
  }

  // Parse path and query string
  size_t queryPos = path.find('?');
  if (queryPos != std::string::npos) {
    query_ = path.substr(queryPos + 1);
    path_ = path.substr(0, queryPos);
  } else {
    path_ = path;
  }

  // URL-decode the path (prevents path traversal via encoded sequences)
  path_ = urlDecode(path_);

  // Security: prevent path traversal (directory climbing)
  if (path_.find("..") != std::string::npos) {
    std::cerr << "[HttpRequest] Path traversal attempt: " << path_ << std::endl;
    return false;
  }

  // Default path to index.html
  if (path_ == "/") {
    path_ = "/index.html";
  }

  // Parse HTTP version
  if (version.size() >= 5 && version.substr(0, 5) == "HTTP/") {
    std::string verStr = version.substr(5);
    size_t dotPos = verStr.find('.');
    if (dotPos != std::string::npos) {
      majorVersion_ = std::stoi(verStr.substr(0, dotPos));
      minorVersion_ = std::stoi(verStr.substr(dotPos + 1));
    }
  }

  return true;
}

bool HttpRequest::parseHeaders(const char *begin, const char *end) {
  const char *current = begin;

  while (current < end) {
    const char *crlf = std::search(current, end, "\r\n", "\r\n" + 2);
    if (crlf == end) {
      return false; // Incomplete header
    }

    // Empty line indicates end of headers
    if (crlf == current) {
      return true; // Headers complete
    }

    // Parse header line: "Key: Value"
    std::string line(current, crlf - current);
    size_t colonPos = line.find(':');
    if (colonPos != std::string::npos) {
      std::string key = line.substr(0, colonPos);
      std::string value = line.substr(colonPos + 1);

      // Trim leading/trailing whitespace
      key.erase(0, key.find_first_not_of(" \t"));
      key.erase(key.find_last_not_of(" \t") + 1);
      value.erase(0, value.find_first_not_of(" \t"));
      value.erase(value.find_last_not_of(" \t") + 1);

      headers_[key] = value;
    }

    current = crlf + 2;
  }

  return false; // Ran out of data before finding end of headers
}

std::string HttpRequest::getHeader(const std::string &field) const {
  auto it = headers_.find(field);
  if (it != headers_.end()) {
    return it->second;
  }
  return "";
}
