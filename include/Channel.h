#pragma once
#include "Timer.h"
#include "noncopyable.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/epoll.h>

class EventLoop;

// Channel is the core event dispatcher. It does NOT own the fd;
// it is given an fd by an external owner (Socket, timerfd, eventfd).
//
// Each Channel belongs to exactly one EventLoop thread and tracks:
//   events_  - which events we are interested in (EPOLLIN, EPOLLOUT, etc.)
//   revents_ - which events actually fired (set by EPollPoller::poll).
// channel是fd事件包装器

// epoll检测到fd有事件时， 由channel分发执行对应回调
// 一个fd对应channel,channel只属于一个EventLoop 单线程事件循环
// channel不拥有fd, fd由于Socket/Timer等外部对象持有， Channel仅仅做事件 管理
class Channel : noncopyable {
public:
  // 无参 无返回回调（写， 关闭， 错误事件）
  using EventCallback = std::function<void()>;
  // 读事件回调， 传入事件到达事件戳
  using ReadEventCallback = std::function<void(Timestamp)>;
  Channel(EventLoop *loop, int fd);
  ~Channel();

  // 只读获取接口
  int fd() const { return fd_; }
  int events() const { return events_; }
  int index() const { return index_; }
  EventLoop *ownerLoop() const { return loop_; }
  uint32_t revents() const { return revents_; }

  // Called by EPollPoller to record the fd's position in the epoll interest
  // list.
  // Poller交互标记， 给epoll轮询器使用
  // index_   标记Channel在Epoll数组里的状态 新增/已注册/待删除】
  //-1 未加入 0 曾经加入 1 fd在epoll中
  void setIndex(int idx) { index_ = idx; }

  // Called by EPollPoller when poll() returns events for this fd.
  // epoll轮询后， 把内核返回的触发事件存入revents_, 供后续分发回调
  void setRevents(uint32_t revents) { revents_ = revents; }
  // 事件状态判断
  bool isNoneEvent() const { return events_ == kNoneEvent; }
  bool isReading() const { return events_ & kReadEvent; } // 是否监听读事件
  bool isWriting() const { return events_ & kWriteEvent; }

  // Enable / disable specific event interests.
  // 事件开关接口
  void enableReading();  // 开启读事件 EPOLLIN | EPOLLPRI
  void disableReading(); // 关闭读事件
  void enableWriting();  // 开启写事件 EPOLLOUT
  void disableWriting(); // 关闭写事件
  void disableAll();     // 清空所有监听事件

  // --- Callback setters ---
  // 回调绑定接口
  void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
  void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
  void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
  void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

  // Called by EventLoop when poll returns events for this fd.
  // Dispatches to the appropriate callback based on revents_.
  // 核心事件分发入口
  void handleEvent(Timestamp receiveTime); // 对外入口， 加线程安全保护

  // Remove this channel from its EventLoop.
  // 将当前Channel从所属EventLoop的epoll中移除， 不再监听事件
  void remove();

  // Tie this channel to a shared_ptr owner to prevent premature destruction.
  // 保活机制
  void tie(const std::shared_ptr<void> &obj);

private:
  // EPOLLIN fd有数据可读
  // EPOLLPRI 紧急数据
  // EPOLLOUT fd缓冲区可写
  static const uint32_t kNoneEvent = 0;
  static const uint32_t kReadEvent = EPOLLIN | EPOLLPRI;
  static const uint32_t kWriteEvent = EPOLLOUT;

  void update(); // 同步events_到epoll, 调用EventLoop::updateChannel()
                 //  最终调用epoll_ctl完成内核监听增 / 改 / 删

  // 带安全保护的事件分发， 配合tie_保活， eventHandling_防重入
  void handleEventWithGuard(
      Timestamp receiveTime); // 真正事件分发，根据revents_ 匹配触发对应回调

  EventLoop *const loop_; // 所属事件循环，固定不可修改
  const int fd_;          // 监听的文件描述符，固定
  uint32_t events_;       // 注册监听的事件掩码
  uint32_t revents_;      // epoll 返回的触发事件
  int index_;             // Poller 状态标记

  bool eventHandling_; // 是否正在执行事件回调（防止重入）
  bool addedToLoop_;   // 是否已经加入 epoll

  std::weak_ptr<void> tie_; // 绑定外部对象，保活用
  bool tied_;               // 是否调用过 tie()

  ReadEventCallback readCallback_;
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};
