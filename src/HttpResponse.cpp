#include "HttpResponse.h"
#include "Buffer.h"
#include <cstdio>

std::string HttpResponse::statusMessageForCode(HttpStatusCode code) const {
    switch (code) {
        case k200Ok: return "OK";
        case k400BadRequest: return "Bad Request";
        case k404NotFound: return "Not Found";
        case k500InternalServerError: return "Internal Server Error";
        default: return "Unknown";
    }
}

void HttpResponse::appendToBuffer(Buffer* output) const {
    char buf[256];

    // Status line
    std::string msg = statusMessage_.empty()
                      ? statusMessageForCode(statusCode_)
                      : statusMessage_;
    std::snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\n",
                  static_cast<int>(statusCode_), msg.c_str());
    output->append(buf);

    // Connection header
    if (closeConnection_) {
        output->append("Connection: close\r\n");
    }

    // Content-Length
    std::snprintf(buf, sizeof(buf), "Content-Length: %zu\r\n", body_.size());
    output->append(buf);

    // Custom headers
    for (const auto& header : headers_) {
        std::snprintf(buf, sizeof(buf), "%s: %s\r\n",
                      header.first.c_str(), header.second.c_str());
        output->append(buf);
    }

    // Empty line before body
    output->append("\r\n");

    // Body
    output->append(body_);
}
