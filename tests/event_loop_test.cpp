#include <gtest/gtest.h>

#include "EventLoop.h"

#include <thread>

// Regression test for ~EventLoop().
//
// The destructor used to `delete poller_` inside its body, but timerQueue_ is a
// unique_ptr *member*, and members are destroyed after the body has run. So
// ~TimerQueue went second: it disables its timerfd Channel, Channel::update()
// forwards that to EventLoop::updateChannel(), and that called through the
// already-freed poller_. Every EventLoop that was ever destroyed segfaulted —
// which also means every EventLoopThread, i.e. TcpServer's IO thread pool, took
// the whole process down when it was torn down.
namespace {

TEST(EventLoopTest, DestroyingALoopDoesNotCrash) {
  // A loop that never armed a timer is enough to reach it: TimerQueue registers
  // its timerfd Channel unconditionally, in its constructor.
  //
  // Own thread, because an EventLoop belongs to the thread that created it, and
  // a __thread guard in EventLoop.cpp aborts outright if a second EventLoop
  // appears in the same thread.
  std::thread thread([] { EventLoop loop; });
  thread.join();
}

} // namespace
