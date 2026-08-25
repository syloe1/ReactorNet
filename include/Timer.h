#pragma once

#include <chrono>
#include <functional>

// Represents a timestamp as microseconds since epoch.
// Used for timer expiration comparison.
class Timestamp { // 时间戳类
public:
  // 系统墙上时钟
  using Clock = std::chrono::system_clock;
  using Microseconds = std::chrono::microseconds;

  Timestamp() : microSecondsSinceEpoch_(0) {}

  explicit Timestamp(int64_t microSecondsSinceEpoch)
      : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

  static Timestamp now() {
    auto now = Clock::now().time_since_epoch();
    return Timestamp(std::chrono::duration_cast<Microseconds>(now).count());
  }

  int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
  // 判断是不是合法时间
  bool valid() const { return microSecondsSinceEpoch_ > 0; }
  // 增加秒数
  Timestamp &operator+=(double seconds) {
    int64_t delta = static_cast<int64_t>(seconds * 1000000);
    microSecondsSinceEpoch_ += delta;
    return *this;
  }
  // 大小比较
  bool operator<(const Timestamp &rhs) const {
    return microSecondsSinceEpoch_ < rhs.microSecondsSinceEpoch_;
  }

  bool operator<=(const Timestamp &rhs) const {
    return microSecondsSinceEpoch_ <= rhs.microSecondsSinceEpoch_;
  }

  bool operator>(const Timestamp &rhs) const {
    return microSecondsSinceEpoch_ > rhs.microSecondsSinceEpoch_;
  }
  // 减法
  //  Returns the difference in seconds.
  double operator-(const Timestamp &rhs) const {
    int64_t diff = microSecondsSinceEpoch_ - rhs.microSecondsSinceEpoch_;
    return static_cast<double>(diff) / 1000000.0;
  }

private:
  int64_t microSecondsSinceEpoch_; // 总微秒
};

// A timer that fires a callback at a given expiration time.
// Supports one-shot (interval == 0) and repeating timers.
class Timer { // 定时器类
public:
  using TimerCallback = std::function<void()>;

  Timer(TimerCallback cb, Timestamp expiration, double interval = 0.0)
      : callback_(std::move(cb)), expiration_(expiration), interval_(interval),
        repeat_(interval > 0.0), sequence_(++s_numCreated_) {}
  // 执行定时器回调
  void run() const {
    if (callback_)
      callback_();
  }
  // 获取下次到期时间
  Timestamp expiration() const { return expiration_; }
  // 是否是重复定时器
  bool repeat() const { return repeat_; }
  // 获取唯一序列号
  int64_t sequence() const { return sequence_; }

  // Restart a repeating timer: advance expiration by interval_.
  // 重置重复定时器到期时间
  void restart(Timestamp now);

  // For ordering in the timer heap (min-heap by expiration).
  // Note: inverted for std::greater / heap ordering.
  bool operator<(const Timer &rhs) const {
    return expiration_ > rhs.expiration_; // Inverted for min-heap convenience
  }
  // 指针比较仿函数
  //  For comparing Timer* in heap operations.
  struct TimerPtrComparator {
    bool operator()(const Timer *a, const Timer *b) const {
      return a->expiration() >
             b->expiration(); // Min-heap: earlier expires first
    }
  };

private:
  const TimerCallback callback_; // 定时任务回调，创建后不可修改
  Timestamp expiration_;         // 下一次到期时间戳
  const double interval_;        // 重复间隔，固定不变
  const bool repeat_;            // 是否循环定时器，固定
  const int64_t sequence_;       // 全局唯一序列号，区分同时到期任务

  static int64_t s_numCreated_; // 全局定时器计数
};
