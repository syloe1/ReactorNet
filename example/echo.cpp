#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpServer.h"

int main() {
  EventLoop loop;
  TcpServer server(&loop, InetAddress(8080));

  server.setMessageCallback(
      [](const TcpConnectionPtr &conn, Buffer *buf, Timestamp) {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg); // Echo back
      });

  server.setThreadNum(4); // 4 worker threads
  server.start();
  loop.loop();
}