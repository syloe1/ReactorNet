#include "Buffer.h"
#include "EventLoop.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

// --- HTTP Server ---
// Usage: ./http_server [port] [docRoot] [numThreads]
// Static file server, docRoot must be explicitly provided
class HttpServer {
public:
  HttpServer(EventLoop *loop, const InetAddress &listenAddr,
             const std::string &docRoot)
      : server_(loop, listenAddr, "HttpServer"), docRoot_(docRoot) {
    server_.setConnectionCallback([](const TcpConnectionPtr &conn) {
      std::cout << "[HTTP] " << conn->peerAddress().toIpPort() << " -> "
                << conn->localAddress().toIpPort() << " is "
                << (conn->connected() ? "UP" : "DOWN") << std::endl;
    });
    server_.setMessageCallback([this](const TcpConnectionPtr &conn, Buffer *buf,
                                      Timestamp) { onMessage(conn, buf); });
  }
  void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
  void start() { server_.start(); }

private:
  void onMessage(const TcpConnectionPtr &conn, Buffer *buf) {
    // HTTP/1.1 keep-alive: handle every complete request already buffered.
    while (true) {
      const char *dataStart = buf->peek();
      const char *dataEnd = dataStart + buf->readableBytes();
      HttpRequest req;
      if (!req.parseRequest(dataStart, dataEnd)) {
        // Incomplete request, wait for more data
        return;
      }
      // Consume one complete request: header ends at \r\n\r\n (GET has no body)
      const char *headerEnd =
          std::search(dataStart, dataEnd, "\r\n\r\n", "\r\n\r\n" + 4);
      if (headerEnd == dataEnd) {
        return;
      }
      buf->retrieve((headerEnd + 4) - dataStart);
      HttpResponse response(false); // keep-alive: do not close
      // Secure path: prevent directory traversal
      std::string filePath = docRoot_ + req.path();
      // Read file
      std::ifstream file(filePath, std::ios::binary);
      if (file.is_open()) {
        std::ostringstream oss;
        oss << file.rdbuf();
        std::string content = oss.str();
        response.setStatusCode(HttpResponse::k200Ok);
        response.setContentType(getMimeType(req.path()));
        response.setBody(content);
      } else {
        // 404 Not Found
        std::string notFoundBody =
            "<html><head><title>404 Not Found</title></head>"
            "<body><h1>404 Not Found</h1><p>"
            "The requested URL was not found on this server."
            "</p></body></html>";
        response.setStatusCode(HttpResponse::k404NotFound);
        response.setContentType("text/html");
        response.setBody(notFoundBody);
      }
      // Serialize response to a Buffer and send (connection stays open)
      Buffer responseBuf;
      response.appendToBuffer(&responseBuf);
      conn->send(responseBuf.retrieveAllAsString());
    }
  }
  static std::string getMimeType(const std::string &path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html")
      return "text/html";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css")
      return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js")
      return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png")
      return "image/png";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg")
      return "image/jpeg";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg")
      return "image/svg+xml";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".ico")
      return "image/x-icon";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json")
      return "application/json";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".txt")
      return "text/plain";
    return "application/octet-stream";
  }
  TcpServer server_;
  std::string docRoot_;
};

void printUsage(const char *prog) {
  std::cout << "Usage:\n"
            << "  " << prog << " port docRoot [numThreads]\n"
            << "\nExamples:\n"
            << "  " << prog
            << " 8080 ../www        Single-threaded HTTP on port 8080\n"
            << "  " << prog
            << " 8080 ../www 4      Multi-threaded HTTP (4 workers)\n"
            << std::endl;
}

int main(int argc, char *argv[]) {
  std::cout << "=== ReactorNet HTTP Static Server Demo ===\n" << std::endl;
  if (argc < 3) {
    printUsage(argv[0]);
    return 1;
  }
  int port = std::stoi(argv[1]);
  std::string docRoot = argv[2];
  int numThreads = 0;
  if (argc >= 4) {
    numThreads = std::stoi(argv[3]);
  }

  EventLoop loop;
  InetAddress listenAddr(port);
  std::cout << "Starting HTTP server on port " << port
            << ", docRoot=" << docRoot << ", threads=" << numThreads
            << std::endl;

  HttpServer server(&loop, listenAddr, docRoot);
  server.setThreadNum(numThreads);
  server.start();
  std::cout << "Server listening on " << listenAddr.toIpPort() << std::endl;
  loop.loop();

  return 0;
}
