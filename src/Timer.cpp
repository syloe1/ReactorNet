#include "Timer.h"

int64_t Timer::s_numCreated_ = 0;

void Timer::restart(Timestamp now) {
  // 重复定时器
  if (repeat_) {
    expiration_ = now;
    expiration_ += interval_;
  } else {
    expiration_ = Timestamp();
  }
}
