#pragma once

#include "Buffer.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "Timer.h"
#include "noncopyable.h"
#include <functional>
#include <memory>
#include <string>

class EventLoop;

// Represents a single TCP connection. Managed via shared_ptr.
// Uses enable_shared_from_this so callbacks can safely extend the connection's
// lifetime.
// 不写访问权限的基类，**默认 private 继承**
class TcpConnection : noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {
  // TcpConnection封装一条已accept成功的TCP连接， 管理conn的 socket fd, channel,
  // buffer
public:
  using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
  using MessageCallback =
      std::function<void(const TcpConnectionPtr &, Buffer *, Timestamp)>;
  using WriteCompleteCallback = std::function<void(const TcpConnectionPtr &)>;
  using CloseCallback = std::function<void(const TcpConnectionPtr &)>;
  // 未注册 ， 已注册， 正在关闭， 已经关闭
  enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

  //   EventLoop *loop_;        // 连接归属的IO线程循环
  //   const std::string name_; // 连接名称
  //   StateE state_;           // 当前连接状态

  //   Socket socket_;                    // TCP套接字封装（持有fd）
  //   std::unique_ptr<Channel> channel_; // fd对应的epoll事件处理器

  //   const InetAddress localAddr_; // 本机地址端口
  //   const InetAddress peerAddr_;  // 客户端对端地址端口

  //   Buffer inputBuffer_;  // 读缓冲区：客户端发来的数据存在这里
  //   Buffer outputBuffer_; // 写缓冲区：待发送给客户端的数据缓存

  // Construct with an already-accepted socket fd and peer address.
  // 循环 连接名字 fd, 本地地址 客户端地址
  TcpConnection(EventLoop *loop, const std::string &name, int sockfd,
                const InetAddress &localAddr, const InetAddress &peerAddr);
  ~TcpConnection();

  // --- Accessors ---
  // 只读访问接口
  EventLoop *getLoop() const { return loop_; }                   // IO线程
  const std::string &name() const { return name_; }              // 名字
  const InetAddress &localAddress() const { return localAddr_; } // 本地地址
  const InetAddress &peerAddress() const { return peerAddr_; }   // 对端地址
  bool connected() const { return state_ == kConnected; }        // 连接是否存活

  // --- Callback setters ---
  void setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
  }
  void setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
  }
  void setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
  }
  void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

  // Called by TcpServer when the connection is established.
  // 激活TCP连接， 正式开始收发数据
  void connectEstablished();

  // Called by TcpServer to start the destruction process.
  // 连接收尾， 释放epoll监听资源
  void connectDestroyed();

  // Send data. Thread-safe: if called from another thread, the actual send
  // is queued to the connection's EventLoop.
  // send string, 发底层二进制数据
  void send(const std::string &message);
  void send(const void *data, size_t len);

  // Initiate an orderly shutdown (finishes writing pending data first).
  // 优雅半关闭
  void shutdown();

  // Force close immediately.
  void forceClose();

private:
  void handleRead(Timestamp receiveTime);
  void handleWrite();
  void handleClose();
  void handleError();
  void sendInLoop(const std::string &message);
  void sendInLoop(const void *data, size_t len);
  void shutdownInLoop();   // 优雅关闭
  void forceCloseInLoop(); // 强制关闭

  EventLoop *loop_;        // 连接归属的IO线程循环
  const std::string name_; // 连接名称
  StateE state_;           // 当前连接状态

  Socket socket_;                    // TCP套接字封装（持有fd）
  std::unique_ptr<Channel> channel_; // fd对应的epoll事件处理器

  const InetAddress localAddr_; // 本机地址端口
  const InetAddress peerAddr_;  // 客户端对端地址端口

  Buffer inputBuffer_;  // 读缓冲区：客户端发来的数据存在这里
  Buffer outputBuffer_; // 写缓冲区：待发送给客户端的数据缓存

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  CloseCallback closeCallback_;
};
