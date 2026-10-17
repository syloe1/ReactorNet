#include "TcpClient.h"
#include "Connector.h"
#include "EventLoop.h"
#include "Socket.h"
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>

namespace reactornet {

TcpClient::TcpClient(EventLoop *loop, const InetAddress &serverAddr,
                     std::string name)
    : loop_(loop), serverAddr_(serverAddr), name_(std::move(name)),
      connector_(std::make_unique<Connector>(loop, serverAddr)) {
  connector_->setNewConnectionCallback(
      [this](int sockfd) { newConnection(sockfd); });
  connector_->setConnectErrorCallback([this](int savedErrno) {
    if (connectErrorCallback_) {
      connectErrorCallback_(savedErrno);
    }
  });
}

TcpClient::~TcpClient() {
  loop_->assertInLoopThread();

  // An attempt may still be in flight, and both of its callbacks capture
  // `this`; stop() cancels the socket, the channel and the deadline.
  connector_->stop();

  if (!connection_) {
    return;
  }

  TcpConnectionPtr conn = std::move(connection_);
  // The close callback points back at this object. It must not survive us:
  // forceClose() below defers through the loop queue and may run later.
  conn->setCloseCallback([](const TcpConnectionPtr &) {});

  if (conn->connected()) {
    // On the loop thread this tears the channel down synchronously and marks
    // the connection disconnected, so the local reference is the last one and
    // ~TcpConnection (which closes the socket) runs before we return.
    conn->connectDestroyed();
  } else {
    // A close is already in flight; the queued forceCloseInLoop completes it
    // on the next loop turn.
    conn->forceClose();
  }
}

void TcpClient::connect() { connector_->start(); }

void TcpClient::disconnect() {
  loop_->runInLoop([this] {
    if (connection_) {
      // Orderly close: flush what is buffered, then shut the write side down so
      // the peer sees EOF and closes its own end.
      connection_->shutdown();
    } else {
      // The handshake has not completed (or never will); cancel it instead.
      connector_->stop();
    }
  });
}

void TcpClient::stop() {
  loop_->runInLoop([this] {
    connector_->stop();
    if (connection_) {
      connection_->forceClose();
    }
  });
}

void TcpClient::setConnectionCallback(ConnectionCallback cb) {
  connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
  messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
  writeCompleteCallback_ = std::move(cb);
}

void TcpClient::setConnectErrorCallback(ConnectErrorCallback cb) {
  connectErrorCallback_ = std::move(cb);
}

void TcpClient::newConnection(int sockfd) {
  loop_->assertInLoopThread();

  // The Connector delivers a connected fd; ask the kernel for both ends rather
  // than trusting the address we dialed (DNS or a wildcard bind may differ).
  sockaddr_in localAddr;
  sockaddr_in peerAddr;
  socklen_t localLen = sizeof(localAddr);
  socklen_t peerLen = sizeof(peerAddr);
  std::memset(&localAddr, 0, sizeof(localAddr));
  std::memset(&peerAddr, 0, sizeof(peerAddr));
  const bool haveLocal =
      ::getsockname(sockfd, reinterpret_cast<sockaddr *>(&localAddr),
                    &localLen) == 0;
  const bool havePeer =
      ::getpeername(sockfd, reinterpret_cast<sockaddr *>(&peerAddr), &peerLen) ==
      0;

  char suffix[64];
  std::snprintf(suffix, sizeof(suffix), "-%s#%d",
                havePeer ? InetAddress(peerAddr).toIpPort().c_str()
                         : serverAddr_.toIpPort().c_str(),
                nextConnId_++);

  TcpConnectionPtr conn = std::make_shared<TcpConnection>(
      loop_, name_ + suffix, Socket(sockfd),
      haveLocal ? InetAddress(localAddr) : InetAddress(),
      havePeer ? InetAddress(peerAddr) : serverAddr_);

  // Publish before connectEstablished(): the connection callback may call
  // disconnect() or stop(), and those must see this connection.
  connection_ = conn;
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(
      [this](const TcpConnectionPtr &c) { removeConnection(c); });

  conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr &conn) {
  loop_->assertInLoopThread();

  if (connection_ == conn) {
    connection_.reset();
  }

  // This runs inside TcpConnection::handleClose, whose Channel is still on the
  // dispatch stack. Destroying the connection here would destroy the Channel
  // mid-dispatch (~Channel asserts against that), so the final teardown is
  // queued. The task captures only `conn`, never `this`, because the client
  // may be destroyed before the task runs.
  loop_->queueInLoop([conn] { conn->connectDestroyed(); });
}

} // namespace reactornet
