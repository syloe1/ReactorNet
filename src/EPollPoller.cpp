#include "EPollPoller.h"
#include "Channel.h"
#include "EventLoop.h"
#include <cstring>
#include <iostream>
#include <unistd.h>
/*
int epollfd_;
ChannelMap channels_;
EventList events_;

*/
// 先构造父类Poller
EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop), epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
  if (epollfd_ < 0) {
    std::cerr << "[EPollPoller] epoll_create1 failed: " << strerror(errno)
              << std::endl;
    std::abort();
  }
}

EPollPoller::~EPollPoller() {
  if (epollfd_ >= 0) {
    ::close(epollfd_);
  }
}
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels) {
  // 阻塞等待就绪IO事件
  int numEvents = ::epoll_wait(epollfd_, events_.data(),
                               static_cast<int>(events_.size()), timeoutMs);
  int savedErrno = errno; // 保存错误码，防止被后续调用覆盖
  Timestamp now(Timestamp::now());

  if (numEvents > 0) {
    fillActiveChannels(numEvents, activeChannels);
    // 如果就绪事件填满数组，自动扩容一倍，避免事件截断丢失
    if (static_cast<size_t>(numEvents) == events_.size()) {
      events_.resize(events_.size() * 2);
    }
  } else if (numEvents < 0) {
    // 出错，EINTR 是被信号中断，属于正常场景，不打印日志
    if (savedErrno != EINTR) {
      std::cerr << "[EPollPoller] epoll_wait error: " << strerror(savedErrno)
                << std::endl;
    }
  }
  // numEvents == 0：超时无事件，直接返回时间戳
  return now;
}
void EPollPoller::updateChannel(Channel *channel) {
  const int index = channel->index();
  int fd = channel->fd();

  if (index == -1 || index == 0) {
    // index=-1：全新Channel，从未加入epoll
    // index=0：曾经加入过，后来被DEL删除
    if (index == -1) {
      // 存入哈希表 fd -> Channel*
      channels_[fd] = channel;
    }
    channel->setIndex(1); // 标记：已在epoll监听列表中
    update(EPOLL_CTL_ADD, channel);
  } else {
    // index=1：fd已经在epoll内
    if (channel->isNoneEvent()) {
      // 当前不监听任何事件，直接从epoll删除
      update(EPOLL_CTL_DEL, channel);
      channel->setIndex(0); // 标记：已移除
    } else {
      // 更新监听事件掩码 EPOLLIN/EPOLLOUT
      update(EPOLL_CTL_MOD, channel);
    }
  }
}
void EPollPoller::removeChannel(Channel *channel) {
  int fd = channel->fd();
  channels_.erase(fd); // 哈希表删除fd映射

  if (channel->index() == 1) {
    // 如果当前还在epoll中，执行DEL
    update(EPOLL_CTL_DEL, channel);
  }
  channel->setIndex(-1); // 重置为全新未注册状态
}

void EPollPoller::update(int operation, Channel *channel) {
  epoll_event event;
  std::memset(&event, 0, sizeof(event));
  event.events = channel->events(); // 要监听的事件掩码
  event.data.ptr = channel;         // 绑定Channel指针

  if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
    // DEL失败一般是fd已经被删除，不用打印错误；ADD/MOD失败才告警
    if (operation != EPOLL_CTL_DEL) {
      std::cerr << "[EPollPoller] epoll_ctl op=" << operation
                << " fd=" << channel->fd() << " failed: " << strerror(errno)
                << std::endl;
    }
  }
}
bool EPollPoller::hasChannel(Channel *channel) const {
  auto it = channels_.find(channel->fd());
  return it != channels_.end() && it->second == channel;
}

// Static factory: creates the platform's default Poller.
Poller *Poller::newDefaultPoller(EventLoop *loop) {
  return new EPollPoller(loop);
}

void EPollPoller::fillActiveChannels(int numEvents,
                                     ChannelList *activeChannels) const {
  for (int i = 0; i < numEvents; ++i) {
    // epoll_event.data.ptr 存入的就是Channel*，直接强转
    Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
    // 把本次触发的事件掩码存入channel.revents_
    channel->setRevents(events_[i].events);
    // 加入活跃列表，EventLoop后续统一分发回调
    activeChannels->push_back(channel);
  }
}