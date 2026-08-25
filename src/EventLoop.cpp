#include "EventLoop.h"
#include "Channel.h"
#include "EPollPoller.h"
#include "Poller.h"
#include "TimerQueue.h"
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/eventfd.h>
#include <unistd.h>

// Thread-local pointer to the EventLoop for the current thread.
// nullptr if no EventLoop has been created on this thread.
//__thread 是GCC拓展关键字， 线程局部存储
// 强制一线程一Loop
__thread EventLoop *t_loopInThisThread = nullptr;
// 最长阻塞10s
const int kPollTimeMs = 10000; // Default poll timeout: 10 seconds

// --- Wakeup mechanism ---
// eventfd 轻量级事件fd. 用于线程间唤醒
// EFD_NONBLOCK非阻塞
// EFD_CLOEXEC exec进程自动关闭fd, 放泄露
int createEventfd() {
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0) {
    std::cerr << "[EventLoop] eventfd creation failed: " << strerror(errno)
              << std::endl;
    std::abort();
  }
  return evtfd;
}
// 全局忽略信号
// SigPipe 终止整个程序， 只是一条连接坏了， 不该把整个服务干掉
//  Ignore SIGPIPE to prevent crashes when writing to closed connections.
class IgnoreSigPipe {
public:
  // 收到 Sigpipe直接忽略， 不要杀进程
  IgnoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }
};

static IgnoreSigPipe initObj;

EventLoop::EventLoop()
    : looping_(false), quit_(false), eventHandling_(false),
      callingPendingFunctors_(false), iteration_(0),
      threadId_(std::this_thread::get_id()), poller_(new EPollPoller(this)),
      timerQueue_(new TimerQueue(this)), wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)) {
  // 校验：当前线程不能已有EventLoop
  if (t_loopInThisThread) {
    std::cerr << "[EventLoop] Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_ << std::endl;
    std::abort();
  }
  t_loopInThisThread = this;

  // 给唤醒fd绑定读回调，开启读监听
  wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleWakeup, this));
  wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
  // 停止监听wakeupfd，从epoll删除
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);

  delete poller_;
  poller_ = nullptr;

  // 清空线程本地指针
  t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
  assert(!looping_);
  assertInLoopThread();
  looping_ = true;
  quit_ = false;

  while (!quit_) {
    activeChannels_.clear();
    // 阻塞10m等待IO事件
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
    ++iteration_;
    // 处理所有就绪IO事件回调
    eventHandling_ = true;
    for (Channel *channel : activeChannels_) {
      // 处理Epoll监测到的fd事件， socket可读， 可写， timerfd定时器到期，
      channel->handleEvent(pollReturnTime_);
    }
    eventHandling_ = false;
    // 执行跨线程投递的任务
    //
    doPendingFunctors();
  }

  looping_ = false;
}

void EventLoop::quit() {
  quit_ = true;
  // Wake up the loop if it's blocked in poll
  if (!isInLoopThread()) {
    wakeup();
  }
}

void EventLoop::runInLoop(Functor cb) {
  if (isInLoopThread()) {
    cb(); // 当前是loop线程，直接执行
  } else {
    queueInLoop(std::move(cb)); // 跨线程，丢入队列
  }
}
// 线程安全投递任务
void EventLoop::queueInLoop(Functor cb) {
  { // 锁持有范围， 最小临界区
    std::lock_guard<std::mutex> lock(mutex_);
    pendingFunctors_.push_back(std::move(cb));
  }
  // Wake up the loop if:
  // - Called from another thread, OR
  // - Called from loop thread while doPendingFunctors is running
  if (!isInLoopThread() || callingPendingFunctors_) {
    wakeup();
  }
}

void EventLoop::updateChannel(Channel *channel) {
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel *channel) {
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel *channel) {
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  return poller_->hasChannel(channel);
}

void EventLoop::assertInLoopThread() {
  if (!isInLoopThread()) {
    abortNotInLoopThread();
  }
}

bool EventLoop::isInLoopThread() const {
  return threadId_ == std::this_thread::get_id();
}

EventLoop *EventLoop::getEventLoopOfCurrentThread() {
  return t_loopInThisThread;
}

void EventLoop::abortNotInLoopThread() {
  std::cerr << "[EventLoop] abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " << std::this_thread::get_id()
            << std::endl;
  std::abort();
}

void EventLoop::wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
  if (n != sizeof(one)) {
    std::cerr << "[EventLoop] wakeup write error: wrote " << n
              << " bytes instead of 8" << std::endl;
  }
}

void EventLoop::handleWakeup() {
  uint64_t one;
  ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
  if (n != sizeof(one)) {
    std::cerr << "[EventLoop] handleWakeup read error: read " << n
              << " bytes instead of 8" << std::endl;
  }
}
// 批量执行跨线程任务
// 取出跨线程EventLoop的任务， 在Loop线程串行执行
void EventLoop::doPendingFunctors() {
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;
  // 交换容器，缩短锁持有时间
  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pendingFunctors_);
  }
  for (const Functor &functor : functors) {
    functor();
  }
  callingPendingFunctors_ = false;
}
