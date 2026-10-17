#pragma once

#include "Buffer.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "Timer.h"
#include "Transport.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace reactornet {

class EventLoop;

// Represents a single TCP connection. Managed via shared_ptr.
// Uses enable_shared_from_this so callbacks can safely extend the connection's
// lifetime.
// TcpConnection 就是 Transport 所描述的东西：一条带生命周期与字节流的连接。
// 因此公有继承接口，而不是内部再包一层纯转发的 Transport。
// 注意：只让具体类继承 enable_shared_from_this；接口若也继承会让
// shared_from_this() 二义，编译失败（RFC-0003 §8.2）。
class TcpConnection : public Transport,
                      public std::enable_shared_from_this<TcpConnection> {
  // TcpConnection封装一条已accept成功的TCP连接， 管理conn的 socket fd, channel,
  // buffer
public:
  using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = Transport::ConnectionCallback;
  using MessageCallback = Transport::MessageCallback;
  using WriteCompleteCallback = Transport::WriteCompleteCallback;
  using CloseCallback = Transport::CloseCallback;
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
  TcpConnection(EventLoop *loop, const std::string &name, Socket socket,
                const InetAddress &localAddr, const InetAddress &peerAddr);
  ~TcpConnection() override;

  // --- Transport accessors ---
  // 只读访问接口
  uint64_t streamId() const override { return kDefaultStreamId; }
  EventLoop *getLoop() const override { return loop_; }    // IO线程
  const std::string &name() const override { return name_; } // 名字
  const InetAddress &localAddress() const override {
    return localAddr_;
  } // 本地地址
  const InetAddress &peerAddress() const override {
    return peerAddr_;
  } // 对端地址
  bool connected() const override {
    return state_ == kConnected;
  } // 连接是否存活

  // --- Callback setters ---
  void setConnectionCallback(ConnectionCallback cb) override {
    connectionCallback_ = std::move(cb);
  }
  void setMessageCallback(MessageCallback cb) override {
    messageCallback_ = std::move(cb);
  }
  void setWriteCompleteCallback(WriteCompleteCallback cb) override {
    writeCompleteCallback_ = std::move(cb);
  }
  void setCloseCallback(CloseCallback cb) override {
    closeCallback_ = std::move(cb);
  }

  // Called by TcpServer when the connection is established.
  // 激活TCP连接， 正式开始收发数据
  void connectEstablished() override;

  // Called by TcpServer to start the destruction process.
  // 连接收尾， 释放epoll监听资源
  void connectDestroyed() override;

  // Send data. Thread-safe: if called from another thread, the actual send
  // is queued to the connection's EventLoop.
  // send string, 发底层二进制数据
  void send(const std::string &message) override;
  void send(const void *data, size_t len) override;

  // Initiate an orderly shutdown (finishes writing pending data first).
  // 优雅半关闭
  void shutdown() override;

  // Force close immediately.
  void forceClose() override;

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
  // send()/shutdown()/forceClose()/connected() read this from any thread while
  // the loop thread writes it (handleClose, connectEstablished, ...), so it is
  // atomic. The state machine's transitions still all happen on the loop
  // thread; only the reads are cross-thread.
  std::atomic<StateE> state_; // 当前连接状态

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

} // namespace reactornet
