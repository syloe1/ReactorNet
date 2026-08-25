#pragma once

#include "Timer.h"
#include "noncopyable.h"
#include <unordered_map>
#include <vector>

class Channel;
class EventLoop;

// Abstract base class for I/O multiplexing.
// Concrete implementation: EPollPoller.
// 抽象基类 接口层
// Poller是IO多路复用的统一抽象， 屏蔽底层epoll / poll / select差异
/*
管理所有channel与 fd映射关系
阻塞等待内核IO事件
把触发事件的Channel收集给EventLoop
提供结构增删改  监听channel
*/
class Poller : noncopyable {
protected:
  using ChannelList = std::vector<Channel *>;
  using ChannelMap = std::unordered_map<int, Channel *>; // O(1)

public:
  // = 0 代表纯虚函数
  virtual void updateChannel(Channel *channel) = 0; // add / update fd 监听事件
  virtual void removeChannel(Channel *channel) = 0;
  // 阻塞轮询， 等待内核IO事件， 把就绪的Channel填入activeChannels
  virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
  // 基类兜底返回false, 子类EpollPoller会重写
  virtual bool hasChannel(Channel *channel) const { return false; }
  // 工厂方法
  static Poller *newDefaultPoller(EventLoop *loop);

  virtual ~Poller() = default;

protected:
  explicit Poller(EventLoop *loop) : ownerLoop_(loop) {}

  // const修饰指针， 指针一旦初始化， 不能再指向别的EventLoop对象
  EventLoop *const ownerLoop_;
};
