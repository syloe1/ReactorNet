# RFC-0003：可插拔 Transport 抽象层 v0.1

| 项目 | 内容 |
|---|---|
| RFC 编号 | 0003 |
| 标题 | 可插拔 Transport 抽象层 |
| 状态 | Draft（待 review） |
| 日期 | 2026-10-06 |
| 里程碑 | RPC M1 · 网络层补齐 |
| 对应 issue | #58 |
| 依赖 | 无 |
| 被依赖 | #59 #60 #61 #62 #63 #64 #65 #66 |
| 接口版本 | `v0.1` |

本文档定义 ReactorNet 网络层的**传输抽象**：一条连接在应用代码眼里应当长什么样，以及 TCP 如何迁移到这个抽象上。

本文档**不实现 QUIC**。QUIC 只作为论证材料出现（§12），用来说明这个抽象为何成立、在哪里会别扭。抽象先用 TCP 钉死，等 QUIC 真开始写，再由它逼出第二版接口。

本文档不含任何 RPC 帧层设计。分帧格式是 RFC-0001 的事，两者正交：RFC-0001 管「字节怎么摆」，本文档管「字节从哪条管道进出」。

---

## 1. 摘要

现状是：**TCP 的 fd 语义直接焊在了连接层**。`Acceptor` 回调、`TcpServer::newConnection`、`TcpConnection` 构造函数三处都以裸 `int sockfd` 传递，`TcpConnection` 内部同时持有 `Socket socket_` 与 `Channel channel_(loop, sockfd)` 两个共享同一 fd 数字的对象，而运行时读写全部绕过 `Socket`、直接对 `channel_->fd()` 做 `readFd` / `writeFd`。

本文档引入一个纯虚接口 `Transport`，语义单位是**流（byte stream）**而非「连接」：

```
TCP  : 一条 TcpConnection 恰好承载一条流，streamId() 恒为 kDefaultStreamId (= 0)
QUIC : 一个连接承载多条流，每条流是一个 Transport，streamId() 返回 QUIC stream id
```

`TcpConnection` 公有继承 `Transport`，现有回调调用点一行不改。上层（HTTP / RPC 会话）从此只依赖 `TransportPtr`，不依赖 fd，也不知道底层是 TCP 还是 QUIC。

---

## 2. 为什么先写这份文档

与 RFC-0001 §2 同一个理由：**接口是改起来最贵的东西**。

一旦 RPC 会话层、HTTP 层、测试都开始依赖 `Transport` 的形状，改一个虚函数签名就要同时改实现、调用点、测试与 example。在文档里改到满意，比在代码里改便宜一个数量级。

本文档的验收标准不是「作者看懂了」，而是：**另一个人只看这份文档，就能写出 `TcpConnection` 的迁移改动，且改出来的与本文档一致。**

与 RFC-0001 的一处不同：RFC-0001 定义的是线格式，改错了会破坏兼容性；本文档定义的是**进程内接口**，改错了只会让自己加班。因此本文档的纪律可以松一档——不必为「未来」预留无法验证的钩子，宁可等真实需求出现再改（§12.4）。

---

## 3. 术语与约定

| 术语 | 含义 |
|---|---|
| 流（stream） | 一条可靠、有序、双向的字节序列。TCP 连接是流；QUIC stream 也是流 |
| 连接（connection） | 一次四层/传输层会话。TCP 上一个连接 = 一条流；QUIC 上一个连接承载多条流 |
| Transport | 本文档定义的对「一条流」的抽象，见 §7 |
| 裸 fd seam | 以裸 `int` 传递 socket 描述符的接口边界，见 §4.1 |
| 机械改名 | 只改参数类型名、不动方法体的源码改动，见 §10.3 |
| 上游 / 下游 | 上游指应用代码（HTTP / RPC 会话），下游指具体传输实现（TCP / QUIC） |

约定：`MUST` / `MUST NOT` / `SHOULD` / `MAY` 按 RFC 2119 解释。

本文档中「必须在 loop 线程」指**必须在该 Transport 的 `getLoop()` 所属线程**执行——这是 ReactorNet 的 one-loop-per-thread 模型的直接推论。

---

## 4. 现状：抽象缺失的具体位置

### 4.1 三个裸 fd seam

新连接从内核 accept 出来之后，fd 以裸 `int` 穿过三层才变成对象：

```
Socket::accept() 返回 int
   ↓ 裸 int
Acceptor::NewConnectionCallback(int sockfd, const InetAddress&)   include/Acceptor.h:16
   ↓ 裸 int
TcpServer::newConnection(int sockfd, const InetAddress&)          include/TcpServer.h:86
   ↓ 裸 int
TcpConnection(EventLoop*, name, int sockfd, local, peer)          include/TcpConnection.h:48
```

**问题不是「用了 int」，而是这段路径上 fd 的所有权不明确**：accept 出来后到 `TcpConnection` 构造完成之前，谁负责在异常路径上关闭它？现在靠的是「Acceptor 里若无回调就 `::close(connfd)`」这类分散约定，没有单一所有者。

### 4.2 `Socket` 的半废弃状态

`Socket`（`include/Socket.h`）是一个完整的 RAII 类：move-only，析构时 `::close`，提供 `bind` / `listen` / `accept` / `setsockopt` 族 / `shutdownWrite` / `close`。

但 `TcpConnection` 只用了它三件事——`setKeepAlive`、`setTcpNoDelay`（`src/TcpConnection.cpp:21-22`）、`shutdownWrite`（`src/TcpConnection.cpp:167`）。**实际的读写在三个地方绕过了它，直接对 `channel_->fd()` 操作**：

| 位置 | 代码 | 性质 |
|---|---|---|
| `src/TcpConnection.cpp:56` | `inputBuffer_.readFd(channel_->fd(), &savedErrno)` | 读路径绕过 Socket |
| `src/TcpConnection.cpp:79` | `outputBuffer_.writeFd(channel_->fd(), &savedErrno)` | 写路径绕过 Socket |
| `src/TcpConnection.cpp:116` | `::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, ...)` | 直接系统调用 |

于是 `TcpConnection` 同时持有 `Socket socket_` 和 `Channel channel_`（`src/TcpConnection.cpp:12-13`），**两者共享同一个 fd 数字**。fd 的生命周期由 `Socket::~Socket()` 负责，而 epoll 的注册/注销由 `Channel` 负责——同一份资源的两个所有者，靠「它们恰好不会打架」维持正确性。

`Socket` 类还缺客户端能力：**没有 `connect`，没有 `read` / `write`**。现有抽象是纯服务端形态。

### 4.3 `Buffer` 与 fd 的耦合

`Buffer::readFd(int fd, int* savedErrno)` 与 `writeFd(int fd, int* savedErrno)`（`include/Buffer.h:36,38`）直接吃裸 fd。这是缓冲层与 fd 最深的耦合点。

其中 `readFd` 的实现（`src/Buffer.cpp:57-85`）用**两段 `iovec` 一次 `readv`**：第一段指向 buffer 的可写区，第二段是栈上 64 KiB 的 `extrabuf`。这是为了避免「大包一来就扩容」的性能优化，`tests/buffer_test.cpp` 有专门覆盖该分支的用例。**任何解耦方案都必须保住这个优化**，否则是性能回归。

另有一处已知的封装泄漏：`Buffer::data()`（`include/Buffer.h:50`）返回整个底层 `std::vector` 的 const 引用，**包含了 prependable 区与 writable 区**，而不是只有可读区。它被 `tests/buffer_test.cpp` 依赖，本文档不动它（§5 Non-Goals）。

---

## 5. 目标与非目标

### 5.1 目标

1. 定义一个传输抽象接口，使应用代码不再接触 fd、不再依赖 TCP 具体类型。
2. 使「换一种传输」在应用层是**零改动**——只换实现类。
3. 把 §4.1 的三个裸 fd seam 收敛干净，让 fd 的所有权单一化。
4. 用 TCP 迁移验证该接口确实可用，且现有 44 个测试保持全绿。

### 5.2 非目标

本轮**明确不做**，写在这里是为了防止 PR 膨胀：

- **QUIC 的任何实现与依赖引入。** 仓库是 Zero Dependencies 的，真接 QUIC 必然引入 crypto 库，那是独立的构建/依赖决策（§13 第 7 条）。
- **RPC 帧层、序列化、连接池。** 属 RFC-0001 及后续 issue。
- **Poller / 事件源可插拔化。** 把 epoll 换成 io_uring、或让 QUIC 自带事件循环接进来，是另一个抽象层次的问题（§13 第 4 条）。`EventLoop.cpp:48` 硬编码 `new EPollPoller` 而没走已有的 `Poller::newDefaultPoller` 工厂（`src/EPollPoller.cpp:109-111`），这是**启用既有工厂**，不是设计新抽象，单独开 issue。
- **定时器 API。** `EventLoop` 至今没有公开定时器接口（`timerQueue_` 是 private、无 accessor，`include/EventLoop.h:93`），而 QUIC 重度依赖定时器。本文档只记录这个约束，不设计接口（§13 第 1 条）。
- **客户端 / connect 路径。** `Socket` 无 `connect`，现有抽象是服务端形态。
- **接受侧的统一抽象（`TransportFactory`）。** 见 §10.4。
- **`Buffer::data()` 的封装泄漏。** 被测试依赖，不动。
- **`src/TcpConnection.cpp:116` 的 `getsockopt` 收敛。** 超出「只补 read/write」的约定，见 §11.3。

---

## 6. 抽象单位：流，不是连接

这是本文档最重要的一个决定，也是最容易做错的一个。

### 6.1 三种方案

| | A. 连接级 Transport + 每调用带 stream_id | B. **每流一个 Transport**（选用） | C. Transport + 独立的 TransportStream 两层 |
|---|---|---|---|
| TCP 成本 | 每次 `send` / 回调多一个恒为 0 的参数 | 0（`streamId()` ≡ 0） | TCP 要伪造一条 stream，多一个对象 |
| QUIC 表达力 | 连接级操作顺畅 | 天然映射 QUIC stream | 最忠实 QUIC |
| 上游代码 | 每处都要处理用不到的 id | 统一：一条流就是一个 Transport | 要管理两级对象 |
| 连接级语义（握手 / GOAWAY / 迁移） | 有位置放 | **没有位置**（需另建连接对象） | 有位置放 |

### 6.2 结论：选 B

1. **应用真正消费的是「一条有序字节流」，而这正是 QUIC stream 的定义。** A 把「连接」当抽象单位，然后给每条消息贴一个 TCP 永远用不到的 id——那是把 QUIC 的内部结构泄漏进公共 API。这是典型的「为抽象而抽象」。
2. **B 让 TCP 路径零开销、零语义改动**：一条 `TcpConnection` 就是一条流，不需要任何适配层。
3. C 的被拒理由：两层结构只有在应用真的需要**连接级操作**时才有回报，而那属于未来的 `QuicConnection` 抽象（§12.3）。现在引入只会让 TCP 侧凭空多出一层没有行为的壳。

### 6.3 保留的门缝

接口上**保留 `streamId()` 访问器**（TCP 返回 `kDefaultStreamId`），但**不做参数化的 `send` / 回调**。

保留它是因为：上层若某天需要对同一连接内的多条流做 demux（例如多路复用的 RPC 客户端），读一个访问器就够了，不需要改签名。成本为零，所以留着。

不做参数化是因为：那个参数在 TCP 上恒为 0，在 QUIC 上又不足以表达完整语义（流还有独立的流控、半关闭、单向/双向之分，见 §13 第 2 条）。**一个既无信息量又不充分的参数，不如不给。**

---

## 7. Transport 接口

### 7.1 接口全文

新增 `include/Transport.h`（纯接口，无 `.cpp`，`CMakeLists.txt` 的 `SOURCES` 无需新增条目）：

```cpp
#pragma once

#include "InetAddress.h"
#include "Timer.h"
#include "noncopyable.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
  virtual void shutdown() = 0;    // 有序半关闭：写完 pending 再 SHUT_WR
  virtual void forceClose() = 0;  // 立即关闭

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
```

### 7.2 为什么接口上没有 fd / Socket

`fd()` 是 TCP 专属概念。QUIC 没有「一条流一个 fd」这回事——一个 QUIC 连接共享一个 UDP fd，多条流复用同一个内核 socket。

**任何出现在接口上的 fd 都会立刻破坏可插拔性**：它会让 QUIC 实现要么返回一个无意义的 fd，要么让所有调用点都得先判断「这个 transport 有没有 fd」。这是「不让 TCP 概念泄漏到接口」最直接的演示。

### 7.3 为什么地址访问器在接口上

`example/echo_server.cpp` 与 `example/http_server.cpp` 用 `conn->peerAddress().toIpPort()` 打日志，`localAddress()` 同理。

放上接口是**合理的**，不是迁就现有代码：TCP 与 QUIC 都有对端四元组（QUIC 的流继承其所属连接的对端地址）。这不是 TCP 专属概念，与 fd 有本质区别。

---

## 8. 所有权与生命周期

### 8.1 结论：公有继承

```cpp
class TcpConnection : public Transport,
                      public std::enable_shared_from_this<TcpConnection>
```

注意 `public Transport`——仓库里 `noncopyable` 那种不写访问权限的基类是**默认私有继承**（`include/TcpConnection.h:19` 有注释说明这点），不能照抄。`noncopyable` 由 `Transport` 传递即可，去掉直接基类。

**为什么不组合**（`TcpConnection` 内部持有 `unique_ptr<Transport>`）：

1. `TcpConnection` **本来就是接口所描述的东西**——一条带生命周期与字节流的连接。组合版本要么把 `inputBuffer_` / `outputBuffer_` / 状态机复制进一个 `TcpTransport`（重复实现），要么加一层纯转发、零行为的壳（GoF Wrapper 反模式）。这正是「过度设计」的标准形态。
2. `shared_ptr<TcpConnection>` 隐式上调为 `shared_ptr<Transport>` **共享同一控制块**：无第二次引用计数、无双重所有权。回调调用点 `connectionCallback_(shared_from_this())` **一行都不用改**。
3. 需要 TCP 细节时，上游可 `std::dynamic_pointer_cast<TcpConnection>(tp)`。

组合方案的代价是：上游拿到的 `TransportPtr` 和连接表里的 `TcpConnectionPtr` 是两个类型，回调里要转换。对「可插拔」这个叙事来说，这层间接恰好是要消除的东西。

### 8.2 `enable_shared_from_this` 的陷阱

**只让 `TcpConnection` 继承 `std::enable_shared_from_this<TcpConnection>`，绝不能让 `Transport` 也继承它。**

若接口与具体类都继承（哪怕是不同模板实参），`shared_from_this()` 会二义，**编译失败**。

因此 `Transport` 保持纯虚、不含任何生命周期机器；需要 `TransportPtr` 时由具体类上调生成临时 `shared_ptr<Transport>`（临时生存期覆盖整个调用表达式，安全）。

保留别名 `using TcpConnectionPtr = std::shared_ptr<TcpConnection>;`，向后兼容。

---

## 9. 回调注册与线程契约

### 9.1 用 setter，不用构造注入

接口上是**纯虚 setter**，存储留在具体类（`TcpConnection` 已有 4 个回调成员，零新增）。

决定性理由：`tests/integration_test.cpp:361` 把用户回调**包装后**再注册（统计存活连接数），构造注入做不到「构造后替换 / 包装」。`TcpServer` 同样要在构造后把自己的 `removeConnection` 装成 close 回调（`src/TcpServer.cpp:103-107`）。

次要理由：仓库现有风格（`Channel` / `TcpConnection` / `TcpServer`）全是 `setXxxCallback`。

**不强行共用存储**：QUIC 实现将来可能有不同的桥接需求（msquic 是回调式的，见 §12.2），setter 只约束接口，不约束存储形态。

### 9.2 线程契约

| 方法 | 线程要求 |
|---|---|
| 除下列二者外的全部方法 | MUST 在 `getLoop()` 所属线程调用 |
| `send()` | 线程安全，内部 `runInLoop` 回本线程 |
| `forceClose()` | 线程安全，内部 `queueInLoop` 回本线程 |

这是对现有 `TcpConnection` 行为的**如实记录**，不是新设计——现有实现的 `send`（`src/TcpConnection.cpp:126-134`）与 `forceClose`（`:171-177`）已经是这个语义。

**这个契约是抽象对 QUIC 最有价值的一处**：msquic 在自己的 worker 线程上回调 stream 事件，`QuicStreamTransport` 必须把这些回调 `runInLoop` 进 `EventLoop`。于是上层看到的仍然是「单线程、所有回调在 loop 线程」的一致语义——**事件模型的差异被实现吸收，不泄漏给上游**。这正是抽象该做的事。

---

## 10. TCP 迁移路径

约束：**现有 44 个测试（buffer 17 / event_loop 9 / timer_queue 11 / integration 6 / smoke 1）必须保持全绿**，`-Wall -Wextra -Wpedantic` 下零新增 warning。

### 10.1 `TcpConnection`

`include/TcpConnection.h`：

- `:19-20` 类声明改为 §8.1 的形式。
- `:24-29` 四个回调 typedef 改为 `= Transport::Xxx`；`TcpConnectionPtr` 别名保留。
- `:48-49` 构造函数改为取 `Socket`：
  ```cpp
  TcpConnection(EventLoop *loop, const std::string &name, Socket socket,
                const InetAddress &localAddr, const InetAddress &peerAddr);
  ```
- `:54-91` 各访问器/方法加 `override`，并新增 `uint64_t streamId() const override { return kDefaultStreamId; }`。
- `:107-108` 成员声明顺序**保持不变**（`socket_` 在 `channel_` 前），因为它决定初始化顺序，而 `channel_` 的构造依赖 `socket_.fd()`。

`src/TcpConnection.cpp`：

- `:12-13` 初始化列表：
  ```cpp
  socket_(std::move(socket)),
  channel_(new Channel(loop, socket_.fd())),
  ```
  `:21-22` 的 `setKeepAlive` / `setTcpNoDelay` 不动。
- `:56` → `inputBuffer_.readFrom(socket_, &savedErrno)`（见 §11.2）。
- `:79` → `outputBuffer_.writeTo(socket_, &savedErrno)`。
- `:116` → `::getsockopt(socket_.fd(), ...)`（**系统调用保留**，理由见 §11.3）。
- `:167` `socket_.shutdownWrite()` 不变。
- 回调调用点（`:37, :47, :59, :85, :106, :110`）**全部不改**——隐式上调生效。

### 10.2 `Acceptor` 与 `TcpServer`

| 位置 | 现在 | 改造后 |
|---|---|---|
| `include/Acceptor.h:16` | `std::function<void(int, const InetAddress&)>` | `std::function<void(Socket, const InetAddress&)>` |
| `include/TcpServer.h:86` | `void newConnection(int sockfd, const InetAddress&)` | `void newConnection(Socket connSocket, const InetAddress&)` |
| `include/TcpConnection.h:48` | `TcpConnection(..., int sockfd, ...)` | `TcpConnection(..., Socket socket, ...)` |
| `src/TcpServer.cpp:92` | `::getsockname(sockfd, ...)` | `::getsockname(connSocket.fd(), ...)`，**必须在 move 之前** |

`src/Acceptor.cpp:57-64`：拿到 `connfd` 后 `if (cb) cb(Socket(connfd), peerAddr); else ::close(connfd);`。**EMFILE 兜底分支（`:68-79`）不动**——那里操作的是 `idleFd_`，仍是裸 fd，属于 Acceptor 内部实现细节，不跨越接口边界。

`Socket` 可 move、不可拷贝，作为值参数经 `std::function` / `std::bind` 传递会正确 move 到 `TcpConnection` 成员。

`include/TcpServer.h:83` 的连接表可改为 `std::map<std::string, TransportPtr>`——`name()` / `getLoop()` / `connectEstablished` / `connectDestroyed` 都在 `Transport` 上，这是「服务端也面向抽象」的演示点。

**收益**：accept 边界上不再有裸 `int`，fd 从「一个没有明确所有者的整数」变成「`Socket` RAII 唯一持有」。

### 10.3 测试与 example 的机械改名（7 处）

因为 `Transport::ConnectionCallback` 的形参是 `const TransportPtr&`，而 `std::function` **对参数类型不做向下转型**，下列签名的类型名必须改。**方法体一行不动**——因为 `send` / `connected` / `peerAddress` / `localAddress` / `shutdown` 全都在 `Transport` 上。

- `tests/integration_test.cpp`：`:330`、`:349-350`、`:362`、`:490`、`:679`
- `example/echo_server.cpp`：`:15`、`:21`
- `example/http_server.cpp`：`:11`、`:21`、`:26`、`:33`

建议在文件顶部加 `using TransportPtr = Transport::TransportPtr;`，参数改为 `const TransportPtr&`。

**行为零变化。** 这批改动属于 `feat: 引入 Transport` 那个 PR 的一部分，不是新增测试、也不是 fix，因此与「测试 PR 后 rebase」的习惯不冲突。

这是本次迁移**唯一**触碰现有测试文件的地方。若坚持一行测试都不碰，回调就只能留在 `TcpConnectionPtr` 上——那样上游仍必须写 `TcpConnection` 类型，「可插拔」是装饰性的。本文档选择不这么做。

### 10.4 未做：接受侧的统一抽象

本文档**不**把接受侧泛化成 `TransportFactory`（让 UDP+QUIC 也能喂进 `TcpServer`）。

理由：接受机制天然是 transport-specific 的——TCP 是 listen fd + `accept4`；QUIC 是 UDP socket + 异步握手 + 库回调，**根本没有 listen fd**。强行统一会引入一个当前无人使用的抽象。

本轮先把「已建立连接」这一侧的 seam 收敛干净。接受侧的抽象是 QUIC 接入时必须补的一环，列为开放问题（§13 第 2 条）。

---

## 11. `Buffer` 与 `Socket` 的解耦

### 11.1 `Socket` 补充（按「只补 read/write」的约定）

`include/Socket.h` 新增三个薄封装，`src/Socket.cpp` 实现：

```cpp
ssize_t read(void *buf, size_t len);                 // ::read 薄封装
ssize_t write(const void *buf, size_t len);          // ::write 薄封装
ssize_t readv(const struct iovec *iov, int iovcnt);  // ::readv 薄封装
```

**为什么必须补 `readv` 而不只是 `read`**：`Buffer::readFd` 的核心是两段 `iovec` 一次读完（§4.3）。`read(void*, size_t)` **表达不了两段 iovec**，读路径改走它就是性能回归。所以流式 API 必须包含 `readv`。

`read` 作为对称基元一并补上（写路径用 `write`），由独立单测覆盖。

### 11.2 `Buffer` 的兼容策略

**保留 `readFd(int, int*)` / `writeFd(int, int*)` 不动**——`tests/buffer_test.cpp` 直接调它们，17 个用例。

**新增** `readFrom(Socket&, int*)` / `writeTo(Socket&, int*)`，二者共享一个私有核心，避免复制那段易错的 `iovec` 组装与溢出处理逻辑：

```cpp
// 伪码：读路径
ssize_t Buffer::readCore(int fd, Socket *sock, int *savedErrno) {
  // ... 组装 vec[2]：writable 区 + 栈上 extrabuf（逻辑完全不变）...
  ssize_t n = sock ? sock->readv(vec, iovcnt) : ::readv(fd, vec, iovcnt);
  // ... 原有溢出处理逻辑不变 ...
}
ssize_t Buffer::readFd(int fd, int *e)       { return readCore(fd, nullptr, e); }
ssize_t Buffer::readFrom(Socket &s, int *e)  { return readCore(-1, &s, e); }
```

`writeFd` / `writeTo` 同理（`writeTo` 调 `sock.write(peek(), readableBytes())`）。

**为什么不做「`Buffer::readFd` 作为 `Socket::read` 的实现细节」**：两者职责正交——`Buffer` 拥有「分散读 + 增长策略」，`Socket` 拥有系统调用。把一个塞进另一个会造成层次倒挂（容器依赖网络层）。正确做法是**组合**：`Buffer::readFrom(Socket&)` 用 `Socket` 的系统调用包装执行它自己的增长策略。

这样 `TcpConnection` 的 I/O 路径再也不出现裸 fd，而 `Buffer` 被测试的接口面一字未动。

**更轻的替代**（若 review 认为 `readFrom` / `writeTo` 是过度设计）：`TcpConnection` 直接写 `inputBuffer_.readFd(socket_.fd(), ...)`，只把 fd 来源从 `channel_->fd()` 换成 `socket_.fd()`。代价是 §11.1 新增的封装只被单测使用、没有生产调用点。

本文档推荐前者：它让「`Socket` 只补 read/write」这个决策**有实际去处**，不是加了就闲置。

### 11.3 有意留在范围外的点

`src/TcpConnection.cpp:116` 的 `::getsockopt(SO_ERROR)` **仍直接调用系统调用**（fd 来源改为 `socket_.fd()`）。

因为约定是「`Socket` 只补 read/write」，所以**不加 `Socket::error()`**。这是一个已知的、被有意留下的裸系统调用，后续可作独立小 PR 收敛。**如实记录未收敛点，好过假装做完了。**

---

## 12. QUIC 接入的形态论证

本章不实现任何东西，只论证这个抽象在 QUIC 面前站不站得住。**这是本文档最重要的一章**。

### 12.1 抽象成立的地方

将来 `QuicStreamTransport : public Transport`，`streamId()` 返回 QUIC stream id，其余方法与 TCP 完全一致。于是：

- 上游 handler 的签名、`send` / `shutdown` / `forceClose` 的调用、回调四件套 **完全不改**，只换实现类。
- 线程契约（§9.2）把 msquic 的多线程模型封装在实现内，不泄漏。
- `streamId()` 让同一 QUIC 连接的多条流可被区分，而 TCP 无需为此付出参数代价。

### 12.2 两种 QUIC 库与 epoll Reactor 的张力

| | quiche（Rust / FFI，函数式） | msquic（C，回调式） |
|---|---|---|
| 驱动方式 | 你拥有 UDP socket，自己 `recv` 数据报喂进去，它给出待发数据报与 `poll()` 超时 | 库在自己的 worker 线程上回调 stream 事件 |
| 与 epoll Reactor 的契合 | **较高**：UDP fd 可以挂进现有 `Channel` | **冲突**：one-loop-per-thread 模型被打破 |
| 需要补的东西 | 把 quiche 的计时需求接进事件循环 | 每流一次跨线程投递 |

**quiche 路线的阻塞点**：它要求你把计时需求接进事件循环，而 `EventLoop` **至今没有公开定时器 API**（`timerQueue_` 私有、无 accessor，`include/EventLoop.h:93`），且 `kPollTimeMs` 硬编码 10 秒（`src/EventLoop.cpp:19`）。这是 §13 的头号开放问题。

**msquic 路线的阻塞点**：所有回调必须在 `QuicStreamTransport` 里 `loop_->runInLoop(...)` 进 EventLoop。可行，但每流多一次跨线程投递——**这个代价被实现吸收，上游无感**，正是 §9.2 契约的价值所在。

### 12.3 别扭之处（抽象需要后续扩展的点）

1. **连接级生命周期无家可归。** QUIC 握手、GOAWAY、MAX_STREAMS、连接级流控、连接迁移、0-RTT、以及「连接关闭向所有流扇出」——这些**不属于任何一条字节流**。需要一个未来的 `QuicConnection`（**不**实现 `Transport`），由它拥有并扇出到各 `QuicStreamTransport`。这是 §6 选择「每流一个 Transport」时已知的代价。
2. **接受模型不同。** 没有 listen fd，握手也是异步的（§10.4）。
3. **地址稳定性假设被打破。** 连接迁移会改变对端地址，而接口承诺 `const InetAddress& peerAddress()`——隐含「这条流的对端地址是稳定的」。QUIC 下这个承诺不成立。
4. **背压语义不同。** QUIC 流有 flow control；TCP 侧 `outputBuffer_` **无上限**（`src/TcpConnection.cpp:147` 直接 `append`）。统一背压接口需要先有需求。
5. **零依赖政策被打破。** 仓库是 Zero Dependencies 的（纯 Linux 系统调用）。真正的 QUIC 必然引入 msquic/quiche + TLS/crypto 库。**这必须是可选、默认关闭的独立构建组件**，不能污染 `reactornet` 静态库的零依赖属性。

第 5 条在简历叙事里很重要：它证明作者理解「抽象可以先行，依赖决策要单独论证」——而不是先引入一个几万行的 crypto 依赖，再回头想接口该怎么设计。

### 12.4 为什么本轮不在接口里解决它

**没有实现就没有验证。**

接口一旦为了臆想的 QUIC 需求长出 id 参数、连接级回调、定时器钩子，就会同时污染已经稳定的 TCP 路径——这是「为抽象而抽象」。§12.3 列出的五个点里，有四个（1/3/4/5）的正确形态取决于具体 QUIC 库的选择，而库还没选。

正确顺序是：**先用 TCP 把接口钉死，等 QUIC 实现真的写起来，让 QUIC 逼出第二版接口**，届时按 §14 的修订流程改。

这与 RFC-0001 §12 确立的「先改文档再改实现」是同一个纪律。

---

## 13. 开放问题

诚实地列出尚未解决的点。**每条都说明「为什么现在不定」**。

| # | 问题 | 为什么现在不定 |
|---|---|---|
| 1 | **定时器能力**：`EventLoop` 无公开定时器 API，`timerQueue_` 私有（`include/EventLoop.h:93`），`kPollTimeMs` 硬编码 10s（`src/EventLoop.cpp:19`）。QUIC idle timeout / 握手超时 / RPC deadline 都需要 | 它需要独立的 API 设计（返回句柄 vs 裸指针、cancel 语义、跨线程契约），与 Transport 抽象**正交**。仓库已有 issue #20 / #21 在跟这条线。本文档只记录「Transport 对超时有需求」，不给接口 |
| 2 | **连接级多路复用语义**：GOAWAY、MAX_STREAMS、连接级流控、连接关闭向所有流扇出，在「每流一个 Transport」下无归属 | 没有 QUIC 实现，无法判断哪些连接级操作真需要暴露给上游。先建 `QuicConnection` 的需求形态未知（§12.3 第 1 条） |
| 3 | **连接级生命周期与 0-RTT / 连接迁移**：会动摇 `peerAddress()` 的稳定性假设 | 只有 QUIC 才需要，且会影响接口对地址的 `const` 承诺。需实测后再定（§12.3 第 3 条） |
| 4 | **事件源集成**：msquic 自带 worker 线程、quiche 需要把定时器接进 epoll 循环。是否要泛化 `Poller`、或引入通用 event source？ | 已明确排除在本轮外（§5.2）。且这是**实现级**问题，不是接口级问题——`Transport` 接口不必知道事件源长什么样 |
| 5 | **统一错误模型**：现在错误只 `std::cerr`（`src/TcpConnection.cpp:70,92,117-123`），无错误码、无 error 回调。Transport 需要统一 errno（TCP）与 QUIC transport error code | 定义错误模型需要**消费者**——RPC 会话层要区分「可重试 / 不可重试」错误。现在没有消费者，先定义只会长出没人用的枚举。仓库已有 issue #39 在跟 |
| 6 | **背压 / 写缓冲上限**：`outputBuffer_` 无上限；QUIC 有流控 | 与 Transport 抽象正交。TCP 侧本身是已知缺陷，应作为独立 issue（仓库已有 #23）在有压力测试后再做 |
| 7 | **零依赖政策 vs QUIC 三方库** | 属构建 / 依赖决策。等真要实现 QUIC 时单独论证（可选组件、默认关闭）。见 §12.3 第 5 条 |

---

## 14. 对下游 issue 的约束

| Issue | 本文档提供的约束 |
|---|---|
| #59（`Socket` 补 read/write/readv） | §11.1 的三个签名；`readv` 不可省的理由 |
| #60（`Buffer` 增加 Socket 入口） | §11.2 的 `readCore` 结构；`readFd` / `writeFd` 签名与行为 MUST NOT 变；17 个 buffer 用例 MUST 全绿 |
| #61（收敛 accept seam） | §10.2 的四处签名 |
| #62（引入 Transport 接口） | §7 的接口全文、§8 的继承形式、§9 的线程契约、§10.3 的 7 处改名清单 |
| #63 / #64（测试） | §5.1 的验收口径 |
| #65 / #66（文档 / 工厂化） | §5.2 的边界 |

**修改流程**：先改本文档，再改实现。反过来做会让文档变成事后描述，失去「实现依据」的作用。

`static_assert` 类的编译期约束（如 `kDefaultStreamId == 0`）MAY 加在实现里，但常量定义 MUST 与本文档一致。

---

## 15. 验收自检

| 验收标准 | 覆盖位置 |
|---|---|
| 抽象单位的选择有对比、有理由 | §6.1 三方案对比表 + §6.2 结论 |
| 接口全文可直接编译 | §7.1 |
| 「为什么接口上没有 fd」明确写出 | §7.2 |
| 所有权方案有取舍论证 | §8.1 继承 vs 组合 |
| `enable_shared_from_this` 二义陷阱写明 | §8.2 |
| 线程契约成文，且与现状一致 | §9.2 |
| TCP 迁移具体到文件与行 | §10.1 / §10.2 / §10.3 |
| 唯一触碰测试的位置穷举 | §10.3（7 处） |
| `readv` 不可省的理由写出 | §4.3 / §11.1 |
| 向后兼容策略明确 | §11.2（`readFd`/`writeFd` 签名不变） |
| 未收敛点如实标注 | §11.3（`getsockopt`）、§5.2（`Buffer::data()`） |
| QUIC 论证既有成立处也有别扭处 | §12.1 / §12.3 |
| 开放问题逐条给出「为什么现在不定」 | §13 |
| 非目标明确排除，防 PR 膨胀 | §5.2 / §10.4 |

---

## 16. 参考资料

- RFC 2119 — `MUST` / `SHOULD` / `MAY` 的定义
- RFC 9000 — QUIC: A UDP-Based Multiplexed and Secure Transport（§2 的 stream 语义是本文档 §6 选择抽象单位的依据）
- RFC 9002 — QUIC Loss Detection and Congestion Control（§13 第 1 条定时器需求的来源）
- muduo 的 `TcpConnection`：本文档 §10 的迁移路径与其结构同源，差异在于 muduo 未做传输抽象
- Netty 的 `Channel` / `ChannelPipeline`：把「传输」与「处理器链」分离的对照设计；Netty 的 NIO / Epoll / KQueue 多传输实现是「可插拔 transport」的成熟样本
- Go 的 `net.Conn` 接口：极简传输抽象的对照——只有 `Read` / `Write` / `Close` / 地址访问器，不含生命周期回调。本文档的接口更宽是因为 ReactorNet 的生命周期由服务端驱动（`connectEstablished` / `connectDestroyed`），而 `net.Conn` 由使用者驱动
- msquic / quiche 官方文档：§12.2 两种驱动模型的事实依据

## 修订历史

| 版本 | 日期 | 说明 |
|---|---|---|
| v0.1 | 2026-10-06 | 初稿：`Transport` 接口、流 vs 连接的抽象单位论证、TCP 迁移路径、`Buffer`/`Socket` 解耦、QUIC 接入形态论证、7 条开放问题 |
