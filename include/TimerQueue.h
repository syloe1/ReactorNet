#pragma once

#include "Channel.h"
#include "Timer.h"
#include "noncopyable.h"
#include <memory>
#include <set>
#include <vector>

namespace reactornet {

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
  // Thread-safe: may be called from any thread; insertion is posted to the
  // EventLoop thread.
  // 指定绝对到期时间戳
  TimerId addTimer(TimerCallback cb, Timestamp when, double interval);

  // 相对当前now延迟delay秒
  //  Convenience overload: add a timer relative to now.
  TimerId addTimer(TimerCallback cb, double delay, double interval);

  // Cancel a timer. Thread-safe and idempotent: cancelling an expired,
  // already-cancelled, or default-constructed TimerId is a no-op. The handle is
  // checked by both pointer and sequence number, so it can never free a new
  // Timer allocated at a reused address.
  void cancel(TimerId timerId);

private:
  using TimerList = std::vector<Timer *>;
  using ActiveTimerSet =
      std::set<TimerId, TimerIdComparator>; // 有序红黑树集合， 保存所有正在堆里的
                                            // Timer 句柄，logn，用于 cancel()
  // 回调函数， timerfd到期， epoll从读事件，
  void handleRead(Timestamp receiveTime);

  // Move expired timers from heap to expired list, reset timerfd.
  // clean expired timer
  std::vector<Timer *> getExpired(Timestamp now);

  // For repeating timers: restart or delete.
  void reset(const std::vector<Timer *> &expired, Timestamp now);

  bool insert(Timer *timer);

  // Body of cancel(), run on the loop thread. allowRetry handles the one case
  // where the cancel can overtake a still-pending insert: try again after the
  // pending loop work has run, then stop (a dead handle is a no-op).
  void cancelInLoop(TimerId timerId, bool allowRetry);

  EventLoop *const loop_;  // 所属事件循环，永久绑定，禁止跨线程操作
  const int timerfd_;      // Linux内核定时器fd，创建后不变
  Channel timerfdChannel_; // 绑定timerfd，监听到期读事件

  TimerList timers_;            // vector实现最小堆，存储所有定时任务
  ActiveTimerSet activeTimers_; // set红黑树，快速查找Timer，用于cancel取消

  bool callingExpiredTimers_; // 正在执行定时回调

  // The current expired batch, by handle. Lets cancel() tell "this timer is in
  // the batch whose callback is running" apart from "already dead" and from
  // "insert still pending".
  std::set<TimerId, TimerIdComparator> expiredTimers_;

  // Timers cancelled from inside a callback that is part of the current
  // expired batch. They are already out of timers_/activeTimers_, so cancel()
  // cannot delete them immediately — reset() still holds the pointer. Record
  // them here and let reset() delete instead of restart.
  std::set<TimerId, TimerIdComparator> cancelingTimers_;
};

} // namespace reactornet
