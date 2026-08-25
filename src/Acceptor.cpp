#include "Acceptor.h"
#include "EventLoop.h"
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
//   EventLoop *loop_;       // 绑定的主线程主Reactor
//   Socket acceptSocket_;   // 封装listen监听fd
//   Channel acceptChannel_; // listen fd对应的epoll事件处理器
//   NewConnectionCallback newConnectionCallback_; // 新连接回调
//   bool listening_;                              // 是否已经调用listen开启端口
//   int idleFd_; // 预留兜底fd，解决EMFILE fd耗尽崩溃问题
Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr)
    : loop_(loop), acceptSocket_(), acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false), idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
  // 端口复用
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(true);
  // 绑定监听地址 0:0:0:0:port
  acceptSocket_.bind(listenAddr);
  // Channel可读事件绑定当前类handleRead
  acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor() {
  acceptChannel_.disableAll(); // 取消epoll所有事件监听
  acceptChannel_.remove();     // 把fd从epoll红黑树彻底移除
  if (idleFd_ >= 0) {
    ::close(idleFd_);
  }
}

void Acceptor::listen() {
  loop_->assertInLoopThread(); // 强制主线程调用
  listening_ = true;
  acceptSocket_.listen();         // dial 底层syscall, 开启TCp半连接队列
  acceptChannel_.enableReading(); // 注册EPOLLIN, epoll监听新连接事件
}

void Acceptor::handleRead() {
  loop_->assertInLoopThread();
  InetAddress peerAddr; // 存储客户端IP端口

  int connfd = acceptSocket_.accept(&peerAddr);
  // 成功拿到新连接
  if (connfd >= 0) {
    if (newConnectionCallback_) {
      newConnectionCallback_(connfd, peerAddr);
    } else {
      ::close(connfd);
    }
  } else {
    // Handle fd exhaustion: close the idle fd, accept again, then reopen idle
    // fd
    if (errno == EMFILE || errno == ENFILE) {
      std::cerr << "[Acceptor] Too many open files, closing idle fd"
                << std::endl;
      ::close(idleFd_);
      // 再次accept, 现在又空闲fd,可以拿到握手客户端
      idleFd_ = acceptSocket_.accept(&peerAddr);
      if (idleFd_ >= 0) {
        ::close(idleFd_);
      }
      // 重新打开/dev/null, 恢复预留fd, 下次异常继续兜底
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
  }
}
