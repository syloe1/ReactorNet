#include "EventLoopThread.h"
#include "EventLoop.h"

namespace reactornet {

EventLoopThread::EventLoopThread() : loop_(nullptr), exiting_(false) {}

EventLoopThread::~EventLoopThread() {
  exiting_ = true;
  {
    // Publish the quit request under the same lock threadFunc() holds while it
    // clears loop_ and destroys the EventLoop. If the IO thread has already
    // finished, we see nullptr and never touch the closed wakeupFd_; if we get
    // the lock first, threadFunc() cannot reach ~EventLoop() until quit() has
    // returned.
    std::lock_guard<std::mutex> lock(mutex_);
    if (loop_ != nullptr) {
      loop_->quit();
    }
  }
  if (thread_ && thread_->joinable()) {
    // 等待子线程函数threadFunc() 执行完毕， os线程回收
    thread_->join();
  }
}

EventLoop *EventLoopThread::startLoop() {
  // 创建系统线程，执行threadFunc
  thread_ = std::make_unique<std::thread>(&EventLoopThread::threadFunc, this);

  // Block until the thread has created the EventLoop
  // 加锁等待子线程创建EventLoop
  std::unique_lock<std::mutex> lock(mutex_);
  cond_.wait(lock, [this]() { return loop_ != nullptr; });

  return loop_;
}

void EventLoopThread::threadFunc() {
  // Heap allocation lets ~EventLoop() run while mutex_ is held below. A
  // stack-local loop would be destroyed after the lock is released, reopening
  // the window the destructor's quit() must not enter.
  auto loop = std::make_unique<EventLoop>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = loop.get();
    cond_.notify_one();
  }
  // 处理当前线程所有IO事件
  loop->loop();

  {
    // Clear the pointer and destroy the EventLoop under mutex_, the same lock
    // ~EventLoopThread() holds while calling quit(). A non-null loop_ now
    // guarantees the object (and its wakeupFd_) is still alive.
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
    loop.reset();
  }
}

} // namespace reactornet
