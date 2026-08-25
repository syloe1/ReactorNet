#pragma once

#include "Poller.h"
#include <sys/epoll.h>
#include <vector>
// 父类Poller的linux专属实现
//  Epoll-based I/O multiplexing implementation.
class EPollPoller : public Poller {
public:
  explicit EPollPoller(EventLoop *loop);
  ~EPollPoller() override;

  void updateChannel(Channel *channel) override;
  void removeChannel(Channel *channel) override;
  bool hasChannel(Channel *channel) const override;
  Timestamp poll(int timeoutMs, ChannelList *activeChannels) override;

private:
  // epoll_event 数组初始容量
  static const int kInitEventListSize = 16;

  // 底层封装 epoll_ctl，统一处理 ADD/MOD/DEL

  void update(int operation, Channel *channel);

  // 遍历 epoll_wait 返回的 events，填充活跃 Channel 列表
  void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;

  // 存储 epoll_wait 返回的 epoll_event 数组
  using EventList = std::vector<epoll_event>;

  int epollfd_;
  // using ChannelMap = std::unordered_map<int, Channel *>; // O(1)
  ChannelMap channels_; // fd -> Channel*
                        //  using EventList = std::vector<epoll_event>;
  EventList events_;    // epoll_wait result buffer
};
