#include "TcpConnection.h"
#include "EventLoop.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace reactornet {

TcpConnection::TcpConnection(EventLoop *loop, const std::string &name,
                             Socket socket, const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(loop), name_(name), state_(kConnecting),
      socket_(std::move(socket)),               // TCP套接字封装（持有fd）
      channel_(new Channel(loop, socket_.fd())), /// fd对应的epoll事件处理器
      localAddr_(localAddr), peerAddr_(peerAddr) {
  channel_->setReadCallback(
      std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
  channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
  channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
  channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));

  socket_.setKeepAlive(true);
  socket_.setTcpNoDelay(true);
}

TcpConnection::~TcpConnection() { assert(state_ == kDisconnected); }

void TcpConnection::connectEstablished() {
  loop_->assertInLoopThread();
  assert(state_ == kConnecting);
  state_ = kConnected;
  // Tie the channel to this shared_ptr so the connection stays alive
  // while the channel is handling events.
  channel_->tie(shared_from_this());
  channel_->enableReading();

  if (connectionCallback_) {
    connectionCallback_(shared_from_this());
  }
}

void TcpConnection::connectDestroyed() {
  loop_->assertInLoopThread();
  if (state_ == kConnected) {
    state_ = kDisconnected;
    channel_->disableAll();
    if (connectionCallback_) {
      connectionCallback_(shared_from_this());
    }
  }
  channel_->remove();
}

void TcpConnection::handleRead(Timestamp receiveTime) {
  loop_->assertInLoopThread();
  int savedErrno = 0;
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0) {
    if (messageCallback_) {
      messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
  } else if (n == 0) {
    // Peer closed write side. If we have pending output, finish writing
    // before closing. Otherwise close immediately.
    if (outputBuffer_.readableBytes() > 0) {
      state_ = kDisconnecting;
    } else {
      handleClose();
    }
  } else {
    std::cerr << "[TcpConnection] handleRead error: " << strerror(savedErrno)
              << std::endl;
  }
}

void TcpConnection::handleWrite() {
  loop_->assertInLoopThread();
  if (channel_->isWriting()) {
    int savedErrno = 0;
    ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
    if (n > 0) {
      if (outputBuffer_.readableBytes() == 0) {
        // All data written
        channel_->disableWriting();
        if (writeCompleteCallback_) {
          writeCompleteCallback_(shared_from_this());
        }
        if (state_ == kDisconnecting) {
          shutdownInLoop();
        }
      }
    } else {
      std::cerr << "[TcpConnection] handleWrite error: " << strerror(savedErrno)
                << std::endl;
    }
  }
}

void TcpConnection::handleClose() {
  loop_->assertInLoopThread();
  assert(state_ == kConnected || state_ == kDisconnecting);
  state_ = kDisconnected;
  channel_->disableAll();

  TcpConnectionPtr guardThis(shared_from_this());
  if (connectionCallback_) {
    connectionCallback_(guardThis);
  }
  if (closeCallback_) {
    closeCallback_(guardThis);
  }
}

void TcpConnection::handleError() {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
    std::cerr << "[TcpConnection] handleError getsockopt failed: "
              << strerror(errno) << std::endl;
  }
  if (err != 0) {
    std::cerr << "[TcpConnection] SO_ERROR = " << err << " " << strerror(err)
              << std::endl;
  }
}

void TcpConnection::send(const std::string &message) {
  if (state_ == kConnected) {
    if (loop_->isInLoopThread()) {
      sendInLoop(message);
    } else {
      // Capture the shared_ptr, not `this`: the connection can be destroyed
      // between queueing this task and running it (the loop may close it in
      // the meantime), and a raw capture turns that into a use-after-free.
      // Holding a reference here keeps the object alive until sendInLoop
      // returns, which is exactly the lifetime the task needs.
      TcpConnectionPtr self = shared_from_this();
      loop_->queueInLoop([self, message]() { self->sendInLoop(message); });
    }
  }
}

void TcpConnection::send(const void *data, size_t len) {
  send(std::string(static_cast<const char *>(data), len));
}

void TcpConnection::sendInLoop(const std::string &message) {
  loop_->assertInLoopThread();
  if (state_ == kDisconnected) {
    std::cerr << "[TcpConnection] send on disconnected connection" << std::endl;
    return;
  }

  size_t remaining = message.size();
  ssize_t nwritten = 0;
  bool faultError = false;

  // Fast path: only when nothing is queued and no write event is armed. The
  // conditions are about ordering as much as about speed — with bytes already
  // in outputBuffer_, writing this message directly would put it ahead of the
  // older ones. Here the kernel gets it immediately and, in the common case,
  // no EPOLLOUT round trip is needed at all.
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    nwritten = socket_.write(message.data(), remaining);
    if (nwritten >= 0) {
      remaining -= static_cast<size_t>(nwritten);
      if (remaining == 0 && writeCompleteCallback_) {
        // Queued rather than called inline: send() may already be running
        // inside a user callback, and invoking user code from here would make
        // the callback re-enter library state mid-send.
        loop_->queueInLoop(
            std::bind(writeCompleteCallback_, shared_from_this()));
      }
    } else {
      const int savedErrno = errno;
      nwritten = 0;
      // EAGAIN/EWOULDBLOCK is ordinary back-pressure, not an error.
      if (savedErrno != EWOULDBLOCK && savedErrno != EAGAIN) {
        std::cerr << "[TcpConnection] sendInLoop write failed: "
                  << strerror(savedErrno) << std::endl;
        if (savedErrno == EPIPE || savedErrno == ECONNRESET) {
          // The peer is gone; queueing the bytes would only grow a buffer that
          // can never be drained.
          faultError = true;
        }
      }
    }
  }

  if (!faultError && remaining > 0) {
    // Partial write or EAGAIN: buffer what is left and let EPOLLOUT drive it.
    // This is the only case that needs outputBuffer_.
    outputBuffer_.append(message.data() + nwritten, remaining);
    if (!channel_->isWriting()) {
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown() {
  if (state_ == kConnected) {
    state_ = kDisconnecting;
    loop_->runInLoop(
        std::bind(&TcpConnection::shutdownInLoop, shared_from_this()));
  }
}

void TcpConnection::shutdownInLoop() {
  loop_->assertInLoopThread();
  // Only shutdown write if all output data has been sent
  if (!channel_->isWriting()) {
    socket_.shutdownWrite();
  }
}

void TcpConnection::forceClose() {
  if (state_ == kConnected || state_ == kDisconnecting) {
    state_ = kDisconnecting;
    loop_->queueInLoop(
        std::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::forceCloseInLoop() {
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting) {
    handleClose();
  }
}

} // namespace reactornet
