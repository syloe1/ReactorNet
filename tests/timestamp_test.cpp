#include <gtest/gtest.h>

#include "Timer.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <time.h>

using namespace reactornet;

// Tests for issue #37. TimerQueue drives timerfd with CLOCK_MONOTONIC, so
// Timestamp has to come from the same clock. With system_clock a single NTP
// step moves every outstanding deadline: a backwards step fires timers early,
// a forwards one postpones them. These tests pin the clock source down rather
// than just checking that two calls happen to be ordered.
namespace {

// The exact clock timerfd is created with.
int64_t monotonicMicros() {
  timespec ts{};
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return -1;
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace

TEST(TimestampTest, NowIsNonDecreasingAcrossManySamples) {
  constexpr int kSamples = 100000;
  int64_t previous = Timestamp::now().microSecondsSinceEpoch();
  for (int i = 0; i < kSamples; ++i) {
    const int64_t current = Timestamp::now().microSecondsSinceEpoch();
    ASSERT_LE(previous, current) << "sample " << i << " went backwards";
    previous = current;
  }
}

TEST(TimestampTest, NowSharesTheMonotonicClockEpoch) {
  // With system_clock this offset is the number of microseconds since 1970 —
  // years. steady_clock and CLOCK_MONOTONIC both count from boot on Linux, so
  // a same-source implementation stays within a second even if the sample is
  // scheduled between the two calls.
  for (int i = 0; i < 50; ++i) {
    const int64_t mono = monotonicMicros();
    ASSERT_NE(mono, -1);
    const int64_t stamp = Timestamp::now().microSecondsSinceEpoch();
    ASSERT_LT(std::llabs(stamp - mono), 1000000)
        << "Timestamp::now() is not on the CLOCK_MONOTONIC epoch";
  }
}

TEST(TimestampTest, NowAdvancesAtTheSameRateAsTheMonotonicClock) {
  const int64_t stampBefore = Timestamp::now().microSecondsSinceEpoch();
  const int64_t monoBefore = monotonicMicros();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const int64_t stampAfter = Timestamp::now().microSecondsSinceEpoch();
  const int64_t monoAfter = monotonicMicros();

  ASSERT_NE(monoBefore, -1);
  ASSERT_NE(monoAfter, -1);
  const int64_t stampDelta = stampAfter - stampBefore;
  const int64_t monoDelta = monoAfter - monoBefore;
  // 20ms of sleep; 10ms of slack for a loaded runner. A different clock source
  // (or a wall-clock step landing between the samples) breaks this outright.
  EXPECT_NEAR(stampDelta, monoDelta, 10000);
  EXPECT_GE(stampDelta, 1000);
}

TEST(TimestampTest, DefaultIsInvalidWhileNowIsValid) {
  EXPECT_FALSE(Timestamp().valid());
  EXPECT_TRUE(Timestamp::now().valid());
}
