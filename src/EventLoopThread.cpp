#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread() : loop_(nullptr), exiting_(false) {}

EventLoopThread::~EventLoopThread() {
  exiting_ = true;
  if (loop_ != nullptr) {
    loop_->quit();
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
  // 每个线程独有loop
  EventLoop loop;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
    cond_.notify_one();
  }
  // 处理当前线程所有IO事件
  loop.loop();

  // When loop exits
  std::lock_guard<std::mutex> lock(mutex_);
  loop_ = nullptr;
}
