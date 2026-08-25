#pragma once

#include "InetAddress.h"
#include "TcpConnection.h"
#include "noncopyable.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class EventLoop;
class Acceptor;
class EventLoopThread;

// Manages all TcpConnections and supports multi-threaded (multi-reactor)
// operation.
//
// Architecture:
//   - baseLoop_: The main reactor thread (accepts new connections).
//   - subLoops_: Worker reactor threads (handle I/O for accepted connections).
//   - Connections are distributed via round-robin across subLoops_.
// TCP服务顶层入口， 连接管理器， 多Reactor线程池调度器
// 主从Reactor, baseLoop_主线程，只允许Acceptor， 专门监听端口， 接受新TCP连接
// subLoops_从Reactor线程池， N个工作IO线程，
// 每个新连接通过轮询分配到其中一个子Loop,
// 连接的所有读写事件只在归属子线程处理
class TcpServer : noncopyable {
public:
  // 每个IO子线程启动时执行的回调, 用来在线程初始化资源（数据库连接， 定时器等）
  using ThreadInitCallback = std::function<void(EventLoop *)>;
  // 连接建立 / 断开回调
  using ConnectionCallback = TcpConnection::ConnectionCallback;
  // 收到客户端报文回调
  using MessageCallback = TcpConnection::MessageCallback;
  // 输出缓冲区全部发送完毕回调
  using WriteCompleteCallback = TcpConnection::WriteCompleteCallback;

  // loop 主Reactor, listenAddr监听本机Ip + 端口 name服务名字
  TcpServer(EventLoop *loop, const InetAddress &listenAddr,
            const std::string &name = "TcpServer");
  ~TcpServer();

  // --- User-facing settings ---
  // 回调注册接口
  void setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
  }
  void setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
  }
  void setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
  }
  void setThreadInitCallback(ThreadInitCallback cb) {
    threadInitCallback_ = std::move(cb);
  }
  // 设置IO工作线程数量
  void setThreadNum(int numThreads);

  // Start the server. numThreads: 0 = single-threaded, N = N worker threads.
  // 启动服务
  void start();

  // --- Accessors ---
  // 只读查询接口
  const std::string &name() const { return name_; }
  EventLoop *getLoop() const { return baseLoop_; }
  std::vector<EventLoop *> getAllLoops();

private:
  // using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

  using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
  using ConnectionMap = std::map<std::string, TcpConnectionPtr>;
  // 收到新TCP连接， 创建TcpConnection对象， 分配IO线程， 加入全局连接表，
  // 正式启用连接
  void newConnection(int sockfd, const InetAddress &peerAddr);
  // 子线程收到关闭事件， 跨线程投递清理任务到主线程
  void removeConnection(const TcpConnectionPtr &conn);
  // 主线程使用，从map删除连接， 收尾销毁资源
  void removeConnectionInLoop(const TcpConnectionPtr &conn);

  EventLoop *const baseLoop_;          // 主线程主Reactor，永不修改
  const std::string name_;             // 服务名称
  std::unique_ptr<Acceptor> acceptor_; // 端口监听对象，仅主线程使用
  std::atomic<bool>
      started_;    // 原子布尔，标记服务是否已启动，多线程安全防重复start
  int nextConnId_; // 原子布尔，标记服务是否已启动，多线程安全防重复start

  // Thread pool
  int threadNum_;
  std::vector<std::unique_ptr<EventLoopThread>> threadPool_; // IO线程管理容器
  std::vector<EventLoop *>
      subLoops_; // 保存每个线程对应的EventLoop指针，用于轮询分配

  // User callbacks
  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  ThreadInitCallback threadInitCallback_;

  ConnectionMap connections_; // 连接名字 -> TcpConnectionPtr
};
