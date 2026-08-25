#pragma once

#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "noncopyable.h"
#include <functional>

class EventLoop;
// 管理listen监听fd, 只处理TCP握手
//  Accepts new TCP connections on a listening socket.
//  Runs exclusively in the main (base) Reactor thread.
class Acceptor : noncopyable {

public:
  using NewConnectionCallback =
      std::function<void(int sockfd, const InetAddress &peerAddr)>;
  Acceptor(EventLoop *loop, const InetAddress &peerAddr);
  ~Acceptor();
  void setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
  }
  // 开启端口监听
  void listen();
  // 查询监听状态s
  bool listening() const { return listening_; }

private:
  // 客户端发起TCP握手， listen fd变为可读， epoll触发EPOLLIN，
  // Channel调用此函数
  void handleRead();

  EventLoop *loop_;       // 绑定的主线程主Reactor
  Socket acceptSocket_;   // 封装listen监听fd
  Channel acceptChannel_; // listen fd对应的epoll事件处理器
  NewConnectionCallback newConnectionCallback_; // 新连接回调
  bool listening_;                              // 是否已经调用listen开启端口
  int idleFd_; // 预留兜底fd，解决EMFILE fd耗尽崩溃问题
};
