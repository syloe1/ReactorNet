#include "Socket.h"
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// SOCK_CLOEXEC执行exec子进程时自动关闭fd, 避免fd泄露
Socket::Socket() {
  sockfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     IPPROTO_TCP);
  if (sockfd_ < 0) {
    std::cerr << "[Socket] Failed to create socket: " << strerror(errno)
              << std::endl;
    std::abort();
  }
}

Socket::~Socket() { close(); }

void Socket::bind(const InetAddress &addr) {
  // bind(int fd, const sockadd* addr, socklen_t len)
  int ret = ::bind(sockfd_, addr.getSockAddr(), addr.getSockLen());
  if (ret < 0) {
    std::cerr << "[Socket] bind failed: " << strerror(errno) << std::endl;
    std::abort();
  }
}

void Socket::listen(int backlog) {
  // listen（fd, 队列长度）
  int ret = ::listen(sockfd_, backlog);

  if (ret < 0) {
    std::cerr << "[Socket] listen failed: " << strerror(errno) << std::endl;
    std::abort();
  }
}
// 接受新连接
int Socket::accept(InetAddress *peerAddr) {
  sockaddr_in addr;
  socklen_t addrLen = sizeof(addr);
  std::memset(&addr, 0, sizeof(addr));
  int connfd = ::accept4(sockfd_, reinterpret_cast<sockaddr *>(&addr), &addrLen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (connfd >= 0) {
    peerAddr->setSockAddr(addr);
  } else {
    int savedErrno = errno;
    if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK &&
        savedErrno != EINTR) {
      std::cerr << "[Socket] accept failed: " << strerror(savedErrno)
                << std::endl;
    }
  }
  return connfd;
}

void Socket::setReuseAddr(bool on) {
  // 1 开启 0 关闭
  int optval = on ? 1 : 0;
  // setsockopt(fd, 选项层级， 选项名， 参数指针， 参数长度)
  if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) <
      0) {
    std::cerr << "[Socket] setsockopt SO_REUSEADDR failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::setReusePort(bool on) {
  int optval = on ? 1 : 0;
  if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) <
      0) {
    std::cerr << "[Socket] setsockopt SO_REUSEPORT failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::setTcpNoDelay(bool on) {
  int optval = on ? 1 : 0;
  if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) <
      0) {
    std::cerr << "[Socket] setsockopt TCP_NODELAY failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::setKeepAlive(bool on) {
  int optval = on ? 1 : 0;
  if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) <
      0) {
    std::cerr << "[Socket] setsockopt SO_KEEPALIVE failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::setNonBlocking() {
  int flags = ::fcntl(sockfd_, F_GETFL, 0);
  if (flags < 0) {
    std::cerr << "[Socket] fcntl F_GETFL failed: " << strerror(errno)
              << std::endl;
    return;
  }
  if (::fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    std::cerr << "[Socket] fcntl F_SETFL O_NONBLOCK failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::shutdownWrite() {
  if (::shutdown(sockfd_, SHUT_WR) < 0) {
    std::cerr << "[Socket] shutdownWrite failed: " << strerror(errno)
              << std::endl;
  }
}

void Socket::close() {
  if (sockfd_ >= 0) {
    if (::close(sockfd_) < 0) {
      std::cerr << "[Socket] close failed: " << strerror(errno) << std::endl;
    }
    sockfd_ = -1;
  }
}