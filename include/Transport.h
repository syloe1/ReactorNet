#pragma once

#include "InetAddress.h"
#include "Timer.h"
#include "noncopyable.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace reactornet {

class Buffer;
class EventLoop;

// Transport —— 一条「可靠、有序、双向的字节流」。
//
// 语义单位刻意是 **流(stream)** 而非「连接」：
//   - TCP  : 一条 TcpConnection 恰好承载一条流，streamId() 恒为 kDefaultStreamId。
//   - QUIC : 一个 QUIC 连接承载多条流，每条流是一个 Transport，
//            streamId() 返回 QUIC 的 stream id。
// 上游（HTTP / RPC 会话）只依赖 TransportPtr，不知道底层是 TCP 还是 QUIC。
//
// 线程契约：除 send() / forceClose() 外，所有方法 MUST 在 getLoop() 所属线程调用。
// send() / forceClose() 线程安全，内部 runInLoop 回本线程（与 TcpConnection 现状一致）。
//
// 接口上不出现 fd / Socket：fd 是 TCP 专属概念，放上来会立刻破坏可插拔性。
// 需要 TCP 细节的上游可 dynamic_pointer_cast<TcpConnection>。
class Transport : noncopyable {
public:
  using TransportPtr = std::shared_ptr<Transport>;

  using ConnectionCallback = std::function<void(const TransportPtr &)>;
  using MessageCallback =
      std::function<void(const TransportPtr &, Buffer *, Timestamp)>;
  using WriteCompleteCallback = std::function<void(const TransportPtr &)>;
  using CloseCallback = std::function<void(const TransportPtr &)>;

  // 单流 transport（TCP）的 stream id。上游 MUST NOT 据此判断底层类型。
  static constexpr uint64_t kDefaultStreamId = 0;

  virtual ~Transport() = default;

  // --- 标识 ---
  virtual uint64_t streamId() const = 0;
  virtual const std::string &name() const = 0;
  virtual const InetAddress &localAddress() const = 0;
  virtual const InetAddress &peerAddress() const = 0;

  // --- 生命周期（服务端面向，应用代码不调用）---
  virtual bool connected() const = 0;
  virtual void connectEstablished() = 0;
  virtual void connectDestroyed() = 0;
  virtual void shutdown() = 0;   // 有序半关闭：写完 pending 再 SHUT_WR
  virtual void forceClose() = 0; // 立即关闭

  // --- I/O ---
  virtual void send(const std::string &message) = 0;
  virtual void send(const void *data, size_t len) = 0;

  // --- 回调注册 ---
  virtual void setConnectionCallback(ConnectionCallback cb) = 0;
  virtual void setMessageCallback(MessageCallback cb) = 0;
  virtual void setWriteCompleteCallback(WriteCompleteCallback cb) = 0;
  virtual void setCloseCallback(CloseCallback cb) = 0;

  // --- 线程归属 ---
  virtual EventLoop *getLoop() const = 0;
};

static_assert(Transport::kDefaultStreamId == 0,
              "RFC-0003 §7.1 fixes the single-stream id at 0");

} // namespace reactornet
