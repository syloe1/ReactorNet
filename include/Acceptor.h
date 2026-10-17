#pragma once

#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "noncopyable.h"
#include <functional>

namespace reactornet {

class EventLoop;
// 管理listen监听fd, 只处理TCP握手
//  Accepts new TCP connections on a listening socket.
//  Runs exclusively in the main (base) Reactor thread.
class Acceptor : noncopyable {

public:
  // Passes ownership of the accepted socket: Socket is the single RAII owner
  // from accept4() onward, so no exception path can leak the fd.
  using NewConnectionCallback =
      std::function<void(Socket socket, const InetAddress &peerAddr)>;
  Acceptor(EventLoop *loop, const InetAddress &peerAddr);
  ~Acceptor();
  void setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
  }
  // 开启端口监听
  void listen();
  // 查询监听状态s
  bool listening() const { return listening_; }

  // The address the listening socket is actually bound to (getsockname).
  //
  // Semantics: the constructor binds, so this is valid *before* listen() and
  // even when listen() is never called. With a port-0 request the kernel picks
  // the port at bind time, which makes this the only way to learn what it
  // picked — tests use it so they never hardcode a port CI might already be
  // using. If getsockname itself fails, a default-constructed (0.0.0.0:0)
  // address is returned.
  //
  // Thread affinity: call on the acceptor's (base) loop thread, like the rest
  // of the acceptor.
  InetAddress listenAddress() const;

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

} // namespace reactornet
