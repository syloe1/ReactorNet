#include "Buffer.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include <iostream>
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

// --- Echo Server ---
// Usage: ./echo_server [numThreads]
// Default: single-threaded echo server on port 8080
class EchoServer {
public:
  EchoServer(EventLoop *loop, const InetAddress &listenAddr)
      : server_(loop, listenAddr, "EchoServer") {
    server_.setConnectionCallback([](const TcpConnectionPtr &conn) {
      std::cout << "[Echo] " << conn->peerAddress().toIpPort() << " -> "
                << conn->localAddress().toIpPort() << " is "
                << (conn->connected() ? "UP" : "DOWN") << std::endl;
    });
    server_.setMessageCallback(
        [](const TcpConnectionPtr &conn, Buffer *buf, Timestamp) {
          // Echo back whatever we received
          std::string msg = buf->retrieveAllAsString();
          conn->send(msg);
        });
  }
  void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
  void start() { server_.start(); }

private:
  TcpServer server_;
};

void printUsage(const char *prog) {
  std::cout << "Usage:\n"
            << "  " << prog
            << " [numThreads]   Echo server (default: single-thread)\n"
            << "\nExamples:\n"
            << "  " << prog << "        Single-threaded echo on port 8080\n"
            << "  " << prog << " 4      Multi-threaded echo (4 workers)\n"
            << std::endl;
}

int main(int argc, char *argv[]) {
  std::cout << "=== ReactorNet Echo Server Demo ===\n" << std::endl;
  int numThreads = 0;
  if (argc > 1) {
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
      printUsage(argv[0]);
      return 0;
    }
    numThreads = std::stoi(argv[1]);
  }

  EventLoop loop;
  InetAddress listenAddr(8080);
  std::cout << "Starting Echo server on port 8080, threads=" << numThreads
            << std::endl;

  EchoServer server(&loop, listenAddr);
  server.setThreadNum(numThreads);
  server.start();
  std::cout << "Server listening on " << listenAddr.toIpPort() << std::endl;
  loop.loop();

  return 0;
}
