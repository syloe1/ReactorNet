#pragma once

#include "InetAddress.h"
#include "noncopyable.h"

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
