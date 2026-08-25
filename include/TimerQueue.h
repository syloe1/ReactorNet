#pragma once

#include "Channel.h"
#include "Timer.h"
#include "noncopyable.h"
#include <memory>
#include <set>
#include <vector>

class EventLoop;

// Manages a collection of timers using a min-heap.
// Uses timerfd_create for kernel-level timer notification.
//
// All timer operations must happen in the owning EventLoop's thread.
// Timer单个定时任务， timerqueue 定时器管理器
class TimerQueue : noncopyable {
public:
  using TimerCallback = std::function<void()>;
  explicit TimerQueue(EventLoop *loop);
  ~TimerQueue();

  // Add a timer. The callback will be invoked after 'delay' seconds.
  // If interval > 0, the timer repeats every 'interval' seconds.
  // Thread-safe: must be called from the EventLoop thread.
  // 指定绝对到期时间戳
  Timer *addTimer(TimerCallback cb, Timestamp when, double interval);

  // 相对当前now延迟delay秒
  //  Convenience overload: add a timer relative to now.
  Timer *addTimer(TimerCallback cb, double delay, double interval);

  // Cancel a timer. Thread-safe.
  void cancel(Timer *timer);

private:
  using TimerList = std::vector<Timer *>;
  using ActiveTimerSet =
      std::set<Timer *>; // 有序红黑树集合， 保存所有正在堆里的Timer指针，
                         // logn， 用来实现cancel() 取消定时器
  // 回调函数， timerfd到期， epoll从读事件，
  void handleRead(Timestamp receiveTime);

  // Move expired timers from heap to expired list, reset timerfd.
  // clean expired timer
  std::vector<Timer *> getExpired(Timestamp now);

  // For repeating timers: restart or delete.
  void reset(const std::vector<Timer *> &expired, Timestamp now);

  bool insert(Timer *timer);

  EventLoop *const loop_;  // 所属事件循环，永久绑定，禁止跨线程操作
  const int timerfd_;      // Linux内核定时器fd，创建后不变
  Channel timerfdChannel_; // 绑定timerfd，监听到期读事件

  TimerList timers_;            // vector实现最小堆，存储所有定时任务
  ActiveTimerSet activeTimers_; // set红黑树，快速查找Timer，用于cancel取消

  bool callingExpiredTimers_; // 正在执行定时回调，防止cancel重入崩溃
};
