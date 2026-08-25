#pragma once

#include "Timer.h"
#include "noncopyable.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Channel;
class Poller;
class TimerQueue;

// Core event loop. Each thread can have at most one EventLoop instance.
// Drives I/O multiplexing (via Poller), timer execution (via TimerQueue),
// and cross-thread task dispatch.
/*
EventLoop是单线程事件驱动循环， 驱动Poller阻塞等待IO事件， 分发Channel读写
内置TimerQueue管理定时任务
提供跨线程任务投递机制， 其他线程可以把函数丢进loop线程串行执行
一个线程最多只能有一个EventLoop,
io, 定时器， 任务全部在本线程串行执行
One loop per thread, 一个线程最多一个EventLoop, 所有的IO, 回调
， 定时任务都在这个线程串行跑
*/
class EventLoop : noncopyable {
public:
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();

  // Enter the event loop. Blocks until quit() is called.
  // 服务器主循环
  void loop();
  // 退出循环
  //  Signal the loop to stop after the current iteration.
  void quit();

  // --- Thread-safe task submission ---

  // Run cb immediately if in loop thread; otherwise queue it.
  // 如果当前就在 loop 线程 → 直接执行 cb；否则丢队列 + 唤醒 loop
  // 跨线程任务投递
  // 场景： 本线程直接跑， 跨线程入队
  void runInLoop(Functor cb);

  // Queue cb for execution in the loop thread. Safe to call from an  y thread.
  // 不管你是哪个线程，一律放进 pendingFunctors 任务队列，必要时唤醒 loop
  // 线程安全， 任意线程可调用
  // 无条件加入任务队列， 不会立即执行
  void queueInLoop(Functor cb);

  // --- Poller delegation ---
  // Poller套接字管理接口 转发给EpollPoller
  void updateChannel(Channel *channel);
  void removeChannel(Channel *channel);
  bool hasChannel(Channel *channel);

  // --- Thread affinity checks ---
  // 线程归属校验
  void assertInLoopThread();
  bool isInLoopThread() const; // 不在本线程直接崩溃

  // Wake up the event loop (called from other threads).
  void wakeup();

  // --- Accessors ---

  Poller *poller() const { return poller_; } // 获取底层epoll poller指针
  // 不用传入对象， 直接拿到 当前正在执行代码的线程绑定的EventLoop
  static EventLoop *
  getEventLoopOfCurrentThread(); // 基于线程本地存储，
                                 // 获取当前线程绑定的EventLoop

private:
  void handleWakeup();      // wakeupChannel的可读回调
  void doPendingFunctors(); // 执行跨线程投递过来的任务队列pendingFunctors
  // 打印错误并终止程序
  void abortNotInLoopThread();

  using ChannelList = std::vector<Channel *>;

  std::atomic<bool> looping_;      // 是否正在执行loop循环
  std::atomic<bool> quit_;         // 是否请求退出循环
  bool eventHandling_;             // 当前是否正在执行IO事件回调
  bool callingPendingFunctors_;    // 当前是否正在执行跨线程任务队列
  int64_t iteration_;              // 循环迭代次数，用于日志调试
  const std::thread::id threadId_; // 创建本loop的线程ID，永久不变
  Poller *poller_;                 // epoll封装
  std::unique_ptr<TimerQueue>
      timerQueue_; // 定时器管理 是EventLoop内部子组件， 生命周期归EventLopp独有
  Timestamp pollReturnTime_;   // epoll_wait返回的时间戳
  ChannelList activeChannels_; // 本次poll就绪的Channel列表

  // EventLoop线程大部分时间阻塞在epoll_wait(), 休眠等待IO事件，
  //  wakeupFd_ + wakeupChannel_ + pendingFunctors + mutex_
  // Wakeup mechanism  跨线程唤醒
  /*  多Reactor 主从Reactor
  A thread可以使用B thread的EventLoop,
  EventLoop对象可以被跨线程引用
  主线程（main thread）：mainLoop 负责 listen + accept
  新连接到来 → 主线程拿到 conn fd
  主线程把这个连接分配给 子IO线程 worker thread 的 workerLoop
  主线程调用 workerLoop->runInLoop( ... ) ，让workerLoop注册Channel、管理连接
  主线程（A），调用了 worker 线程（B）所属的 workerLoop。
  */
  int wakeupFd_;
  std::unique_ptr<Channel> wakeupChannel_;

  // Pending functors queue (cross-thread task dispatch)
  std::vector<Functor> pendingFunctors_;
  std::mutex mutex_;
};
