#pragma once

#include "InetAddress.h"
#include "noncopyable.h"
#include <sys/types.h>
#include <sys/uio.h>

// 封装socket系统调用
class Socket : noncopyable {
public:
  explicit Socket(int sockfd) : sockfd_(sockfd) {}

  Socket();
  ~Socket();

  Socket(Socket &&other) noexcept : sockfd_(other.sockfd_) {
    other.sockfd_ = -1;
  }
  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      // 关闭当前对象的fd, 释放旧资源
      close();
      // 接管别人的fd
      sockfd_ = other.sockfd_;
      other.sockfd_ = -1;
    }
    return *this;
  }

  int fd() const { return sockfd_; }
  // 绑定IP + 端口
  void bind(const InetAddress &addr);
  // backlog未完成3次握手的连接队列最大长度
  // bind后accept前使用
  void listen(int bakclog = SOMAXCONN);
  // 封装accept4非阻塞接受客户端连接
  int accept(InetAddress *peerAddr);

  // 封装的读写系统调用。失败返回 -1 并保留 errno，由调用方决定如何处理——
  // 非阻塞 IO 下 EAGAIN / EWOULDBLOCK / EINTR 都属正常情况，故此处不打印、
  // 不重试，避免与调用方的错误处理重复。
  ssize_t read(void *buf, size_t len);
  ssize_t write(const void *buf, size_t len);
  // 分散读：一次读入多段缓冲区，避免大包导致读缓冲过早扩容
  ssize_t readv(const struct iovec *iov, int iovcnt);

  void setReuseAddr(bool on);  // 地址复用
  void setReusePort(bool on);  // 端口复用
  void setTcpNoDelay(bool on); // 关闭Nagle算法， 小包直接发送
  void setKeepAlive(bool on);  // TCP保活
  void setNonBlocking();
  void shutdownWrite(); // 关闭发送端
  void close();

private:
  int sockfd_;
};
