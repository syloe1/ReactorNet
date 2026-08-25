#include "Channel.h"
#include "EventLoop.h"
#include <cassert>
#include <iostream>
#include <sys/epoll.h>

Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop), fd_(fd), events_(0), // 初始不监听任何事件
      revents_(0),                      // 本次触发事件空
      index_(-1),                       // -1：未加入epoll
      eventHandling_(false),            // 当前没有在执行回调
      addedToLoop_(false),              // 还没注册到epoll
      tied_(false) {}                   // 未绑定外部shared_ptr保活

Channel::~Channel() {
  assert(!eventHandling_);
  assert(!addedToLoop_);
}
// 事件开关接口
void Channel::enableReading() {
  events_ |= kReadEvent; // 按位或，叠加读掩码 EPOLLIN | EPOLLPRI
  update();
}

void Channel::disableReading() {
  events_ &= ~kReadEvent; // 按位与取反，清除读掩码
  update();
}

void Channel::enableWriting() {
  events_ |= kWriteEvent; // 叠加 EPOLLOUT
  update();
}

void Channel::disableWriting() {
  events_ &= ~kWriteEvent; // 清除 EPOLLOUT
  update();
}

void Channel::disableAll() {
  events_ = kNoneEvent; // 直接置0，不监听任何事件
  update();
}

void Channel::update() {
  addedToLoop_ = true; // addedToLoop是否已加入epoll
  loop_->updateChannel(this);
}

void Channel::remove() {
  assert(isNoneEvent());
  addedToLoop_ = false;
  loop_->removeChannel(this);
}
void Channel::handleEvent(Timestamp receiveTime) {
  std::shared_ptr<void> guard;
  if (tied_) {
    guard = tie_.lock();
    if (guard) {
      handleEventWithGuard(receiveTime);
    }
  } else {
    handleEventWithGuard(receiveTime);
  }
}

void Channel::handleEventWithGuard(Timestamp receiveTime) {
  eventHandling_ = true; // 标记正在处理事件，禁止析构

  // 1. 处理连接关闭 EPOLLHUP
  if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
    if (closeCallback_)
      closeCallback_();
  }

  // 2. 处理错误 EPOLLERR
  if (revents_ & EPOLLERR) {
    if (errorCallback_)
      errorCallback_();
  }

  // 3. 处理读事件：EPOLLIN / EPOLLPRI / EPOLLRDHUP
  if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
    if (readCallback_)
      readCallback_(receiveTime);
  }

  // 4. 处理写事件 EPOLLOUT
  if (revents_ & EPOLLOUT) {
    if (writeCallback_)
      writeCallback_();
  }

  eventHandling_ = false; // 事件处理完毕，解除标记
}

void Channel::tie(const std::shared_ptr<void> &obj) {
  tie_ = obj;   // weak_ptr 托管 shared_ptr的资源， 但 不增加 引用计数
  tied_ = true; // 标记启用保活
}
