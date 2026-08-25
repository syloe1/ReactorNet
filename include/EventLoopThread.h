#pragma once

#include "noncopyable.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class EventLoop;

// Encapsulates a thread that runs an EventLoop.
// The EventLoop is created inside the thread and returned via startLoop().
// EventLoopThread封装一条允许EventLoop的IO线程
class EventLoopThread : noncopyable {
public:
  EventLoopThread();
  ~EventLoopThread();

  // 创建底层std::thread, 执行threadFunc()

  EventLoop *startLoop();

private:
  void threadFunc(); // 线程入口函数

  EventLoop *loop_; // 子线程内的EventLoop指针，主线程通过startLoop获取
  bool exiting_;    // 标记是否要退出循环，析构置true
  std::unique_ptr<std::thread> thread_; // 底层操作系统线程封装
  std::mutex mutex_;                    // 保护loop_、exiting_共享变量
  std::condition_variable cond_;        // 同步：主线程等子线程创建loop完成
};
