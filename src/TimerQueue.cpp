#include "TimerQueue.h"
#include "EventLoop.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sys/timerfd.h>
#include <unistd.h>

//   EventLoop *const loop_;  // 所属事件循环，永久绑定，禁止跨线程操作
//   const int timerfd_;      // Linux内核定时器fd，创建后不变
//   Channel timerfdChannel_; // 绑定timerfd，监听到期读事件

//   TimerList timers_;            // vector实现最小堆，存储所有定时任务
//   ActiveTimerSet activeTimers_; // set红黑树，快速查找Timer，用于cancel取消

//   bool callingExpiredTimers_; // 正在执行定时回调，防止cancel重入崩溃
// 创建定时器fd
int createTimerfd() {
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0) {
    std::cerr << " [TimerQueue] timerfd_create failed: " << strerror(errno)
              << std::endl;
    std::abort();
  }
  return timerfd;
}
// __time_t tv_sec;
struct timespec howMuchTimeFromNow(Timestamp when) {
  int64_t microseconds =
      when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
  // prevent < 0
  if (microseconds < 100) {
    microseconds = 100; // Minimum 100us
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(microseconds / 1000000);
  ts.tv_nsec = static_cast<long>((microseconds % 1000000) * 1000);
  return ts;
}

void resetTimerfd(int timerfd, Timestamp expiration) {
  struct itimerspec newValue;
  struct itimerspec oldValue;
  std::memset(&newValue, 0, sizeof(newValue));
  std::memset(&oldValue, 0, sizeof(oldValue));
  newValue.it_value = howMuchTimeFromNow(expiration);
  // timerfd_settime修改内核timerfd超时时间
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret < 0) {
    std::cerr << "[TimerQueue] timerfd_settime failed: " << strerror(errno)
              << std::endl;
  }
}
// clean buf
void readTimerfd(int timerfd, Timestamp /*now*/) {
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof(howmany));
  if (n != sizeof(howmany)) {
    std::cerr << "[TimerQueue] read timerfd error: reads " << n
              << " bytes instead of 8" << std::endl;
  }
}

TimerQueue::TimerQueue(EventLoop *loop)
    : loop_(loop), timerfd_(createTimerfd()), timerfdChannel_(loop, timerfd_),
      timers_(), callingExpiredTimers_(false) {
  timerfdChannel_.setReadCallback(
      std::bind(&TimerQueue::handleRead, this, std::placeholders::_1));
  timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue() {
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();
  ::close(timerfd_);
  // Clean up all timers
  for (Timer *timer : timers_) {
    delete timer;
  }
}

Timer *TimerQueue::addTimer(TimerCallback cb, Timestamp when, double interval) {
  Timer *timer = new Timer(std::move(cb), when, interval);
  loop_->runInLoop([this, timer]() { insert(timer); });
  return timer;
}

Timer *TimerQueue::addTimer(TimerCallback cb, double delay, double interval) {
  Timestamp when = Timestamp::now();
  when += delay;
  return addTimer(std::move(cb), when, interval);
}

void TimerQueue::cancel(Timer *timer) {
  loop_->runInLoop([this, timer]() {
    auto it = activeTimers_.find(timer);
    if (it != activeTimers_.end()) {
      activeTimers_.erase(it);
      // Don't delete here; the timer will be in timers_ and cleaned up
      // when it would have expired, or we leave it and clean up in dtor.
      // Actually, let's delete immediately for simplicity and safety:
      // 从堆中删除该timer rebuild the heap
      auto heapIt = std::find(timers_.begin(), timers_.end(), timer);
      if (heapIt != timers_.end()) {
        timers_.erase(heapIt);
        std::make_heap(timers_.begin(), timers_.end(),
                       Timer::TimerPtrComparator());
      }
      delete timer;
    }
  });
}

void TimerQueue::handleRead(Timestamp /*receiveTime*/) {
  loop_->assertInLoopThread(); // 线程校验
  Timestamp now = Timestamp::now();
  readTimerfd(timerfd_, now);

  std::vector<Timer *> expired = getExpired(now);

  callingExpiredTimers_ = true;
  for (Timer *timer : expired) {
    timer->run();
  }
  callingExpiredTimers_ = false;

  reset(expired, now);
}
// 去除所有到期定时器
std::vector<Timer *> TimerQueue::getExpired(Timestamp now) {
  std::vector<Timer *> expired;

  while (!timers_.empty() && timers_.front()->expiration() <= now) {
    // Remove from active set if present
    activeTimers_.erase(timers_.front());

    std::pop_heap(timers_.begin(), timers_.end(), Timer::TimerPtrComparator());
    expired.push_back(timers_.back());
    timers_.pop_back();
  }

  return expired;
}

void TimerQueue::reset(const std::vector<Timer *> &expired, Timestamp now) {
  for (Timer *timer : expired) {
    if (timer->repeat()) {
      timer->restart(now);
      insert(timer);
    } else {
      delete timer;
    }
  }

  if (!timers_.empty()) {
    Timestamp nextExpire = timers_.front()->expiration();
    if (nextExpire.valid()) {
      resetTimerfd(timerfd_, nextExpire);
    }
  }
}

bool TimerQueue::insert(Timer *timer) {
  bool earliestChanged = false;
  Timestamp when = timer->expiration();

  if (timers_.empty() || when < timers_.front()->expiration()) {
    earliestChanged = true;
  }

  timers_.push_back(timer);
  std::push_heap(timers_.begin(), timers_.end(), Timer::TimerPtrComparator());
  activeTimers_.insert(timer);

  if (earliestChanged) {
    resetTimerfd(timerfd_, timer->expiration());
  }

  return earliestChanged;
}
