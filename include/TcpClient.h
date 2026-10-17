#pragma once

#include "InetAddress.h"
#include "TcpConnection.h"
#include "noncopyable.h"
#include <functional>
#include <memory>
#include <string>

namespace reactornet {

class Connector;
class EventLoop;

// Client-side entry point, the counterpart of TcpServer.
//
// TcpClient owns a Connector for the handshake and, once it succeeds, one
// TcpConnection for the established session. It deliberately does not retry:
// connect() starts exactly one attempt, and a disconnected client stays down
// (create a new TcpClient to reconnect).
//
// Threading: the object belongs to the loop thread; its destructor and callback
// setters must run there. connect()/disconnect()/stop() hop onto the loop and
// are safe from any thread. The TcpClient must outlive its loop.
class TcpClient : noncopyable {
public:
  using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
  using ConnectionCallback = TcpConnection::ConnectionCallback;
  using MessageCallback = TcpConnection::MessageCallback;
  using WriteCompleteCallback = TcpConnection::WriteCompleteCallback;

  // Called when a connect attempt fails, before any connection exists. The
  // errno is the real cause; ETIMEDOUT means the peer never answered within
  // the Connector's deadline.
  using ConnectErrorCallback = std::function<void(int savedErrno)>;

  TcpClient(EventLoop *loop, const InetAddress &serverAddr, std::string name);
  ~TcpClient();

  // Start one connection attempt. Ignored if one is already in flight or the
  // client is connected.
  void connect();

  // Orderly close: finishes pending output, then shuts the write side down.
  // Before the handshake completes this cancels the attempt instead. The
  // connection callback then fires with connected() == false once the peer
  // closes. A later connect() is still ignored after a successful attempt.
  void disconnect();

  // Cancel an in-flight attempt and close the connection immediately. No
  // pending output is flushed.
  void stop();

  void setConnectionCallback(ConnectionCallback cb);
  void setMessageCallback(MessageCallback cb);
  void setWriteCompleteCallback(WriteCompleteCallback cb);
  void setConnectErrorCallback(ConnectErrorCallback cb);

private:
  void newConnection(int sockfd);
  void removeConnection(const TcpConnectionPtr &conn);

  EventLoop *loop_;
  const InetAddress serverAddr_;
  const std::string name_;
  std::unique_ptr<Connector> connector_;
  TcpConnectionPtr connection_; // loop-thread owned; cleared on close
  int nextConnId_ = 1;

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  ConnectErrorCallback connectErrorCallback_;
};

} // namespace reactornet
