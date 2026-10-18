# ReactorNet 学习指南

## 这是什么？

ReactorNet 是一个 **核心网络层约 2000 行 C++17** 的项目，实现了经典的 **Reactor 模式**（事件驱动 + 非阻塞 I/O），并在其上构建了一个小型 RPC 层（分帧、请求配对、超时、服务注册、raw/JSON 两种 payload 序列化）。它是 [muduo](https://github.com/chenshuo/muduo) 网络库的教学级精简版。**零第三方依赖**，只依赖 Linux 系统调用（epoll、timerfd、eventfd）和 C++ 标准库——JSON writer/parser 也是手写的。

读完这个项目，你将理解：
- Reactor / 多 Reactor 网络模型的核心原理
- Linux epoll 非阻塞 I/O 编程
- C++ RAII、智能指针、线程安全在实际项目中的应用
- 二进制协议分帧、请求 id 配对、超时与错误分类
- 「帧层 / 信封 / payload 序列化」如何分层，以及 JSON 的转义、UTF-8 与递归深度处理

---

## 前置知识检查

在开始前，确保你已经掌握以下内容：

1. **C++ 基础**：类、虚函数、RAII、`shared_ptr`/`unique_ptr`、`enable_shared_from_this`、lambda、`std::thread`、`std::mutex`
2. **Linux 网络编程基础**：socket、bind、listen、accept、TCP 三次握手
3. **I/O 多路复用概念**：select/poll/epoll 是干什么的（不要求熟练掌握 API）

如果以上有欠缺，建议先补上再看代码。

---

## 学习路线（6 个阶段，建议顺序阅读）

```
第 1 层：基础设施
  noncopyable.h → InetAddress → Socket
              ↓
第 2 层：事件循环核心 ★ 最关键
  Channel → Poller → EPollPoller → EventLoop
              ↓
第 3 层：定时器
  Timer → TimerQueue
              ↓
第 4 层：TCP 网络层
  Buffer → TcpConnection → Acceptor → TcpServer → EventLoopThread
              ↓
第 5 层：应用层
  HttpRequest → HttpResponse → example/http_server.cpp
              ↓
第 6 层：RPC 层
  protocol → FrameReader → Codec → Serializer → JsonValue
  → JsonWriter / JsonParser → ServiceRegistry → RpcServer / RpcClient
```

前 5 层是网络库本身；第 6 层是它的第一个真实消费者。先读第 6 层，你会更清楚第 1–4 层为什么要设计成现在这样（例如 `Transport`、`EventLoop::runAfter`、`Buffer` 的 prependable 空间）。

---

## 第 1 层：基础设施（3 个文件）

### 1.1 `include/noncopyable.h`
**5 分钟** — 一个工具基类，禁用拷贝但允许移动。整个项目大量使用它作为基类。

```cpp
class noncopyable {
public:
    noncopyable() = default;
    ~noncopyable() = default;
    noncopyable(const noncopyable&) = delete;            // 禁止拷贝
    noncopyable& operator=(const noncopyable&) = delete; // 禁止赋值
    noncopyable(noncopyable&&) = default;                // 允许移动
    noncopyable& operator=(noncopyable&&) = default;
};
```

**思考题**：为什么网络库中的 Socket、EventLoop 这些类要禁止拷贝？
    Socket、EventLoop 封装独占内核资源（fd、事件上下文），默认浅拷贝会导致多个对象持有同一份资源，析构时双重释放，引发未定义行为

### 1.2 `include/InetAddress.h` + `src/InetAddress.cpp`
**10 分钟** — 对 `sockaddr_in` 的轻量封装。学会：
- 如何用 `inet_pton` / `inet_ntop` 做 IP 地址转换
- `sockaddr_in` 结构体的字段含义（sin_family、sin_port、sin_addr）

### 1.3 `include/Socket.h` + `src/Socket.cpp`
**20 分钟** — RAII 风格的 socket fd 包装器。关键点：

| 方法 | 作用 |
|------|------|
| 构造函数 | `socket()` 创建非阻塞 fd（`SOCK_NONBLOCK \| SOCK_CLOEXEC`） |
| `bind()` / `listen()` | 服务端标准流程 |
| `accept()` | 使用 `accept4` 返回非阻塞 fd |
| `shutdownWrite()` | 半关闭（发送 FIN，但还能读） |
| 析构函数 | `close(fd)` 自动释放 |

**重要设计**：看 `setReuseAddr`、`setReusePort`、`setTcpNoDelay`、`setKeepAlive` 这些 socket option 的设置方式。

**思考题**：`SOCK_CLOEXEC` 和 `SOCK_NONBLOCK` 为什么重要？不用它们会有什么问题？
  1. **SOCK_NONBLOCK**：创建 fd 时直接设为非阻塞，避免 `fcntl` 并发竞争；是事件驱动网络库（epoll）必需。
  2. **SOCK_CLOEXEC**：创建 fd 时带上 FD_CLOEXEC 标志，防止 `fork+exec` 子进程继承 socket fd，造成资源泄漏、端口无法释放。
---

## 第 2 层：事件循环核心（4 个文件）★ 整个项目最关键的一层

**这部分是整个 Reactor 模式的核心，花最多时间在这里。**

### 2.1 `include/Channel.h` + `src/Channel.cpp`
**30 分钟** — Channel 是"事件分发器"：把一个 fd 和它关心的事件（读/写/关闭/错误）以及对应的回调函数绑定在一起。

```cpp
// 核心数据
int fd_;                    // 不拥有 fd，只是引用
int events_;                // 关心的事件（EPOLLIN | EPOLLOUT 等）
int revents_;               // 实际发生的事件（由 Poller 填充）

// 回调函数（由 Channel 的使用者设置）
ReadEventCallback readCallback_;
EventCallback writeCallback_;
EventCallback closeCallback_;
EventCallback errorCallback_;
```

**关键设计：tie 机制**
```cpp
std::weak_ptr<void> tie_;  // 指向 Channel 的"主人"

void Channel::handleEvent() {
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();  // 提升为 shared_ptr
        if (guard) handleEventWithGuard();          // 安全处理事件
    }
}
```
TcpConnection 通过 `tie()` 把自己的 shared_ptr 绑定到 Channel，防止在处理事件时连接对象被销毁。

**思考题**：为什么 Channel 不拥有 fd？tie 机制解决了什么问题？
  遵循职责分离，fd 由 TcpConnection/Socket RAII 管理生命周期；Channel 仅作为 fd 的事件分发包装，负责向 Poller 注册事件、分发回调，只引用 fd，不负责创建与关闭，
  遵循职责分离，fd 由 TcpConnection/Socket RAII 管理生命周期；Channel 仅作为 fd 的事件分发包装，负责向 Poller 注册事件、分发回调，只引用 fd，不负责创建与关闭，
### 2.2 `include/Poller.h` + `include/EPollPoller.h` + `src/EPollPoller.cpp`
**30 分钟** — I/O 多路复用的抽象。Poller 是抽象基类，EPollPoller 是 epoll 的具体实现。

```
Poller（抽象接口）
  ├── poll()            → 等待事件，返回活跃的 Channel 列表
  ├── updateChannel()   → 添加/修改 fd 的监听事件
  └── removeChannel()   → 停止监听 fd
        ↑
EPollPoller（epoll 实现）
  ├── epoll_create1()   → 创建 epoll 实例
  ├── epoll_ctl()       → ADD/MOD/DEL fd
  └── epoll_wait()      → 阻塞等待事件
```

**关键实现细节**：
- `EPollPoller::poll()` 中，`events_` vector 会随活跃 fd 数量增长而扩容
- 用 `std::unordered_map<int, Channel*>` 维护 fd → Channel 的映射
- 有一个 `fillActiveChannels()` 辅助方法将 epoll_event 数组转成 Channel 列表，并设置每个 Channel 的 `revents_`

**思考题**：为什么要用抽象基类 Poller，而不是直接使用 epoll？这样做的好处是什么？
> 面向接口编程， 方便切换poll版本golang中使用interface实现

### 2.3 `include/EventLoop.h` + `src/EventLoop.cpp`
**45 分钟** — 事件循环本身。每个线程最多一个 EventLoop（用 `__thread` TLS 变量断言）。

```
EventLoop::loop() 的主循环：
while (!quit_) {
    activeChannels = poller_->poll(timeout);   // ① 等待事件
    for (Channel* ch : activeChannels) {
        ch->handleEvent();                     // ② 分发事件
    }
    doPendingFunctors();                       // ③ 执行跨线程任务
}
```

**关键设计点**：

1. **wakeup 机制**：使用 `eventfd` 实现跨线程唤醒。当其他线程调用 `queueInLoop()` 时，往 eventfd 写入数据，epoll 检测到该 fd 可读，loop 被唤醒并执行任务。

2. **跨线程任务队列**：`std::vector<Functor> pendingFunctors_` 受 `std::mutex` 保护。`queueInLoop()` 跨线程投递任务，`doPendingFunctors()` 在主线程执行它们。

3. **`runInLoop()` vs `queueInLoop()`**：
   - `runInLoop()`：如果当前在 loop 线程，直接执行；否则调用 `queueInLoop()` 排队
   - `queueInLoop()`：总是排队到 pendingFunctors 队列

4. **线程安全检查**：`assertInLoopThread()` 通过 TLS 变量检查当前是否在正确的线程中。

5. **定时器门面**：`runAt(when, cb)` / `runAfter(seconds, cb)` / `runEvery(seconds, cb)` 直接委托私有的 `timerQueue_`，返回 `TimerId`；`cancelTimer(id)` 取消。三者都可从任意线程调用——`TimerQueue::addTimer()` 内部经 `runInLoop()` 把堆插入投递到 loop 线程，语义与 `runInLoop()` 一致：loop 线程立即注册，其他线程入队并唤醒。回调永远在 loop 线程执行。`TimerId` 同时携带指针和全局序列号，过期或重复取消都是 no-op。

**思考题**：
- 为什么需要 eventfd？不能直接往 pendingFunctors 队列里加任务然后等自然唤醒吗？
- `doPendingFunctors()` 使用了 swap 技巧——为什么不用直接遍历原 vector？
- 画出 EventLoop 一个完整循环的时序图。

---

## 第 3 层：定时器（2 个文件）

### 3.1 `include/Timer.h` + `src/Timer.cpp`
**15 分钟** — 两个类：

- **Timestamp**：微秒级时间点，提供算术运算符（相减、加秒数）。时钟源是 `std::chrono::steady_clock`（Linux 上即 `CLOCK_MONOTONIC`），与 `timerfd_create()` 用的是同一个时钟。因此它表示的是「自某个任意纪元（通常是开机）起的时长」，**不是墙上时间**：不要拿它格式化日期，也不要和 `system_clock` 的值混用；只有两个 Timestamp 的差值有意义。这样 NTP 调时（阶跃或缓慢调整）不会让已经排好的超时提前或推迟。字段名 `microSecondsSinceEpoch_` 是历史遗留，改名会扩散到很多调用点，所以只保留注释说明。
- **Timer**：一个定时器对象，包含回调函数、过期时间、重复间隔、全局序列号。

注意 `Timer::operator<` 的反向比较：这是为了让 `std::priority_queue` 变成最小堆（最早过期的在堆顶）。

### 3.2 `include/TimerQueue.h` + `src/TimerQueue.cpp`
**30 分钟** — 基于 **最小堆** 的定时器管理器。关键设计：

```
TimerQueue 的数据结构：
  std::vector<Timer*> heap_;           // 最小堆（按过期时间）
  int timerfd_;                        // Linux timerfd，集成到 epoll
  Channel timerfdChannel_;             // timerfd 的 Channel

工作流程：
  ① addTimer() → 插入堆中
  ② 如果新定时器比堆顶更早过期 → timerfd_settime() 重置 timerfd
  ③ epoll_wait 检测到 timerfd 可读 → handleRead()
  ④ handleRead() → 取出所有已过期的定时器 → 执行回调
  ⑤ 如果是重复定时器 → restart() 重新插入堆
```

**为什么用 timerfd 而不是自己轮询？**
timerfd 是 Linux 内核提供的定时器接口——内核会在定时器到期时通过 epoll 通知你，不需要在每次 `loop()` 迭代中手动检查堆顶。这让你可以在 `epoll_wait` 中同时等待 I/O 事件和定时器事件。

添加定时器示例（经 `EventLoop` 的门面，见 2.3）：
```cpp
TimerId timeout = loop.runAfter(3.0, []{ printf("3 秒到了！\n"); });   // 单次
loop.cancelTimer(timeout);                                            // 需要时取消

TimerId heartbeat = loop.runEvery(5.0, []{ printf("每 5 秒发送心跳\n"); }); // 重复
loop.cancelTimer(heartbeat);
```

**思考题**：`TimerQueue` 内部用裸指针 `Timer*` 管理堆，这在你刚看完 EventLoop 用智能指针的代码后可能让你困惑。想一想：这里的裸指针安全吗？为什么？

---

## 第 4 层：TCP 网络层（7 个文件）

### 4.1 `include/Buffer.h` + `src/Buffer.cpp`
**20 分钟** — 非连续的读写缓冲区。三区设计：

```
+------------------+---------------------------+------------------+
| prependable (8B) | readable (数据已到未消费) | writable (可写) |
+------------------+---------------------------+------------------+
                   ↑ readerIndex_             ↑ writerIndex_
```

**亮点**：`readFd()` 使用 `readv` + 64KB 栈上临时缓冲区。
```
如果 socket 收到大量数据：
  ① readv(fd, [buffer_.writableArea, stackBuf])  // 两段 iovec
  ② 如果 stackBuf 用上了 → 把溢出的部分 append 到 buffer
  ③ 如果 stackBuf 没用上 → 数据全在 buffer_ 里
```
这避免了频繁扩容——只在实际需要时才扩展 buffer。

**思考题**：为什么要保留 8 字节的 prependable 区域？HTTP 解析时怎么用到它？

### 4.2 `include/TcpConnection.h` + `src/TcpConnection.cpp`
**45 分钟** — 整个项目最复杂的类，代表一个 TCP 连接。

**状态机**：
```
kConnecting → kConnected → kDisconnecting → kDisconnected
                               ↓
                          forceClose() 可以跳过 kDisconnecting
```

**关键设计**：

1. **`enable_shared_from_this`**：TcpConnection 继承自它。回调中需要持有连接的 shared_ptr 防止提前销毁。

2. **线程安全的 send()**：
   ```
   send() 被调用
     ├── 如果在 IO 线程 → 直接 sendInLoop()
     ├── 如果在其他线程 → queueInLoop(sendInLoop) 排队到 IO 线程
   ```

3. **sendInLoop() 的逻辑**：
   ```
   if (输出缓冲区为空 && 可以直接写) {
       直接 write() → 没写完的部分 append 到 outputBuffer_
       开启 EPOLLOUT 监听 → 等待可写事件
   } else {
       append 到 outputBuffer_
   }
   ```
   - 可写事件触发 `handleWrite()` → 继续写 → 写完关闭 EPOLLOUT

4. **优雅关闭 vs 强制关闭**：
   - `shutdown()`：等 output buffer 写完再 `shutdownWrite()` → 进入 kDisconnecting
   - `forceClose()`：直接进入 kDisconnecting，不等

5. **Channel 回调注册**：
   - `handleRead()`：从 socket 读数据，调用 `messageCallback_` 通知上层
   - `handleWrite()`：写完成后的处理
   - `handleClose()`：连接关闭
   - `handleError()`：连接错误

**思考题**：
- `send()` 为什么要区分是否在 IO 线程？不区分会怎样？
- 画出 TcpConnection 从建立（connectEstablished）到关闭（connectDestroyed）的完整生命周期。
- `handleClose()` 和 `handleError()` 的区别是什么？

### 4.3 `include/Acceptor.h` + `src/Acceptor.cpp`
**15 分钟** — 监听 socket 的管理器。比较简单：
- 持有 acceptSocket_（listening socket）+ acceptChannel_
- 当有新连接到达，回调 `newConnectionCallback_(newFd, peerAddr)`

**亮点：idleFd 机制**
```cpp
int idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
```
当 accept 失败且是 EMFILE（进程 fd 耗尽）时：
1. 关闭 idleFd_（腾出一个 fd 位置）
2. accept 新连接
3. 立即关闭新连接（我们不接受这个连接）
4. 重新打开 /dev/null 填充 idleFd_
这样服务器不会崩溃，只是优雅地拒绝新连接。

**思考题**：为什么要用 idleFd 这个技巧？不用会怎样？这个技巧有什么局限性？

### 4.4 `include/EventLoopThread.h` + `src/EventLoopThread.cpp`
**15 分钟** — 把 EventLoop 包装到一个独立线程中。

```cpp
class EventLoopThread {
    EventLoop* loop_;             // 线程中运行的 loop
    std::thread thread_;          // 工作线程
    std::mutex mutex_;
    std::condition_variable cv_;  // 用于等待 loop 初始化完成
};
```

`startLoop()` 创建线程、阻塞等待 EventLoop 创建完成、返回 loop 指针。

**思考题**：为什么需要 `condition_variable` 来同步？不能直接返回 loop 指针吗？

### 4.5 `include/TcpServer.h` + `src/TcpServer.cpp`
**30 分钟** — 最上层的服务端类，整合所有组件。

**单 Reactor vs 多 Reactor**：

```
单 Reactor (threadNum_ = 0)：
  baseLoop 处理一切：
    ├── Acceptor（accept 新连接）
    └── 所有 TcpConnection 的 I/O

多 Reactor (threadNum_ = N)：
  baseLoop（主 Reactor）：
    └── Acceptor（只 accept）
         ↓ round-robin 分发
  subLoop[0]（从 Reactor 1）：
    └── TcpConnection A, TcpConnection D, ...
  subLoop[1]（从 Reactor 2）：
    └── TcpConnection B, TcpConnection E, ...
  subLoop[2]（从 Reactor 3）：
    └── TcpConnection C, TcpConnection F, ...
```

**连接分发流程**（`newConnection()` 方法）：
```
1. Acceptor 回调 → TcpServer::newConnection(sockfd, peerAddr)
2. 选择一个 IO 线程（round-robin: next_ = (next_ + 1) % N）
3. 在选中的 loop 中创建 TcpConnection
4. 调用 connectEstablished() → Channel 开始监听
5. 加入 ConnectionMap 管理
```

**连接清理流程**（`removeConnection()` / `removeConnectionInLoop()`）：
```
1. TcpConnection 的 handleClose 回调 → TcpServer::removeConnection()
2. queueInLoop(baseLoop) → removeConnectionInLoop()
3. 从 ConnectionMap 移除
4. queueInLoop(ioLoop) → connectDestroyed() → Channel 移除、Socket 关闭
```

**关键设计**：清理发生在 baseLoop 中（不在 IO 线程中），避免竞态。

**思考题**：
- 为什么 round-robin 而不是最少连接数？什么场景下 round-robin 不够好？
- `removeConnection()` 为什么要绕到 baseLoop 去执行？
- 如果用 4 个线程，一个客户端发送大量数据，其他客户端会受影响吗？

### 4.6 `include/Connector.h` + `TcpClient.h`（客户端侧）
**20 分钟** — 服务端有 `Acceptor`，客户端对应的角色是 `Connector`：非阻塞 `connect` + 在 `Channel` 上等 `EPOLLOUT`，处理 `EINPROGRESS` 与失败重试。`TcpClient` 把它和 `TcpConnection` 的生命周期包在一起，提供 `connect()` / `disconnect()` 与连接回调。

**关键点**：连接建立前，`TcpConnection` 还不存在；`TcpClient` 负责在 `Connector` 成功回调里创建它，并保证这些回调都在自己的 loop 线程执行。`RpcClient` 正是复用这套客户端设施。

**思考题**：`connect()` 返回 `EINPROGRESS` 时，为什么要等 `EPOLLOUT`？`SO_ERROR` 在这里起什么作用？

---

## 第 5 层：应用层（HTTP 示例）

### 5.1 `include/HttpRequest.h` + `src/HttpRequest.cpp`
**15 分钟** — 极简的 HTTP/1.0 请求解析器。
- 解析请求行（GET /path HTTP/1.0）
- 解析 Headers
- URL 解码（`%XX` 和 `+`）
- 路径安全检查（拒绝 `..` 防止目录穿越）
- 默认路径：`/` → `/index.html`

### 5.2 `include/HttpResponse.h` + `src/HttpResponse.cpp`
**10 分钟** — HTTP/1.1 响应构建器。
- 设置状态码（200/400/404/500）
- 设置 Content-Type、Content-Length
- 序列化为 Buffer
- 默认 keep-alive，`closeConnection_ = true` 时才发 `Connection: close`

### 5.3 `example/echo_server.cpp` + `example/http_server.cpp`
**20 分钟** — 把网络层组件串起来。两个独立可执行文件：

**EchoServer**：
```cpp
// 收到消息 → 原样发回
void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    conn->send(buf);  // 一行搞定
}
```

**HttpServer**：
```cpp
// 收到 HTTP 请求 → 读文件 → 构建 HTTP/1.1 响应 → 发送（keep-alive 不断连）
void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    // 一个 Buffer 里可能攒了多个请求，循环逐个处理
    while (true) {
        HttpRequest req;
        if (!req.parseRequest(buf->peek(), buf->peek() + buf->readableBytes()))
            return;  // 请求不完整，等下一批数据

        // 消费一个完整请求（GET 无 body，头以 \r\n\r\n 结束）
        const char* end = std::search(buf->peek(), buf->peek() + buf->readableBytes(),
                                      "\r\n\r\n", "\r\n\r\n" + 4);
        buf->retrieve((end + 4) - buf->peek());

        HttpResponse resp(false);        // false = keep-alive，不断连
        resp.setBody(readFile(docRoot + req.path()));
        Buffer respBuf;
        resp.appendToBuffer(&respBuf);
        conn->send(respBuf.retrieveAllAsString());  // 不 shutdown，连接保持
    }
}
```

这两个示例只依赖核心库；`example/rpc_echo_server.cpp` 与
`example/rpc_echo_client.cpp` 依赖 `reactornet_rpc`，在第 6 层展开。

---

## 第 6 层：RPC 层（`rpc/`）

**约 2 小时** — RPC 层是核心网络库的第一个消费者：它只拿到 `TransportPtr`，不直接碰 socket/epoll。建议按下面的顺序读。

### 6.1 阅读顺序

```
protocol.h / protocol.cpp        16 字节帧头 + status 枚举
        ↓
frame_reader.h / .cpp            TCP 字节流 → 完整帧（每连接一份状态）
        ↓
codec.h / raw_codec.cpp          帧 body 的信封：service/method + payload
        ↓
serializer.h                      payload ↔ JsonValue 的接口
        ├── raw_serializer.cpp    id 0：任意字节，v0.1 默认
        └── json_serializer.cpp   id 1：JSON 文本
                ↓
json_value.h                     通用对象模型
json_writer.h / json_parser.h    JsonValue ↔ JSON 文本
        ↓
service_registry.h               名字 → handler 表（start() 后只读）
        ↓
rpc_server.h / rpc_client.h      服务端分发、客户端配对/超时/错误映射
```

### 6.2 帧格式：16 字节帧头

```
偏移  0    2    3    4    5      6         7        8      12     16
     magic ver type flags status serializer reserved req_id body_len body...
      2B   1B   1B   1B    1B      1B        1B       4B     4B    变长
```

- `magic = "RN"`，`version = 1`，`type` 区分 REQUEST/RESPONSE；
- `request_id` 把响应配回请求，`0` 非法；
- `body_len` 是长度前缀；解码器必须**先校验上限再分配内存**（RFC-0001 §4.4）；
- **偏移 6 是 `serializer`**：`0` = raw、`1` = JSON。它原本是保留字节，`0` 恰好延续旧行为，所以没有升 `version`。响应必须回显请求的值；客户端发现不一致会关闭连接，而不是把错误格式的 payload 交给业务。

body 布局（`Codec` 负责）：

```
REQUEST   [u16 svc_len][service][u16 method_len][method][payload]
RESPONSE  [payload]                     // status 在帧头，不在 body
```

### 6.3 Codec 与 Serializer 的分工（#42）

这是 RPC 层最重要的设计点：

| 组件 | 负责 | 不负责 |
|---|---|---|
| `FrameReader` | 字节流 → 完整帧、长度上限 | body 里的名字与 payload 含义 |
| `Codec` | 帧 body 的信封：service/method 的位置 | payload 是 JSON 还是别的 |
| `Serializer` | payload 字节 ↔ `JsonValue` | 帧头、长度前缀、连接 |

好处：换 payload 格式不需要改帧布局；换帧层也不需要改 JSON。`Serializer` 只有两个上线标识：`id()` 写进帧头，`name()`（`"raw"` / `"json"`）只给日志和配置用。

### 6.4 JSON 实现（#43）

`JsonValue` 是一个 tagged struct，不是 `std::variant`：

```cpp
enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
using Array = std::vector<JsonValue>;
using Object = std::vector<std::pair<std::string, JsonValue>>;  // 保留插入顺序
```

- 整数用 `int64_t`、浮点用 `double` 分开存，`isInteger()` 区分，避免 `42` round-trip 成 `42.0`；
- 对象是 `vector<pair>`，编码顺序确定、可测试；代价是查找 O(n)（payload 很小）；
- `JsonWriter`：`std::to_chars` 输出最短 round-trip 数字；非有限值拒绝；转义引号、反斜杠与控制字符；非法 UTF-8 拒绝；
- `JsonParser`：递归下降，严格 RFC 8259——只认 4 个空白字符、拒绝尾随垃圾、处理 `\uXXXX`/代理对/`\u0000`、校验 UTF-8、数字溢出 `int64` 时退回 `double`；
- **深度上限默认 64，在递归前检查**，所以 `[[[[...` 的恶意输入不会打爆栈；
- 失败时输出参数保持不变，调用方可以安全地丢弃整条消息。

`JsonSerializer` 只是把 `JsonWriter` / `JsonParser` 套进 `Serializer` 接口，并检查 `kMaxBodyLen`。

### 6.5 服务端与客户端

**服务端**（`RpcServer`）：

```
TcpServer 收到字节
  → FrameReader 切帧
  → 帧头校验（magic/version/type/status/serializer/body_len）
  → Codec 解出 service/method/payload
  → ServiceRegistry 查表
  → handler(payload) 在连接所属 IO 线程执行
  → Codec 组 RESPONSE，Serializer id 回显
```

- 注册表在 `start()` 后只读，多 IO 线程并发查表不需要锁；
- handler 抛出的异常在分发边界被捕获，映射成 `INTERNAL_ERROR`，连接保持可用；
- 不认识的 `serializer` id 回 `UNSUPPORTED_SERIALIZER`（`0x0D`），连接继续可用。

**客户端**（`RpcClient`）：

- `Call()` 阻塞，**不能在 loop 线程调用**（会死锁）；`AsyncCall()`/`CallAsync()` 可从任意线程调用；
- 在途请求放在 `pendings_`，只在客户端自己的 loop 线程访问，因此不需要锁；
- `Call(request, timeoutSeconds)` 给每个请求挂一个定时器，到期返回 `kTimeout`，连接保留；迟到响应按已超时 id 丢弃；
- 错误统一成 `Response{status, payload}`：服务端状态原样透传，传输问题 → `kConnectionClosed`，发送前参数非法 → `kBadRequest`，超时 → `kTimeout`。

### 6.6 对照示例读

`example/rpc_echo_server.cpp` 注册 `Echo.Echo`，用 `JsonParser` 解析请求、`JsonWriter` 重新编码；`example/rpc_echo_client.cpp` 构造 `{"message": ..., "client": ...}`，两端都 `setSerializerId(kSerializerJson)`。跑起来后抓包或打印 payload，可以直接看到帧头与 JSON 文本。

**思考题**：
- 为什么 `Codec` 不应该解析 JSON？如果让它解析，换 protobuf 时要改哪些代码？
- 为什么 `serializer` 放在帧头，而不是 body 的第一个字节？
- `JsonParser` 为什么必须在递归**之前**检查深度？在递归之后检查会怎样？
- 客户端为什么能安全地让多个线程同时调用 `Call()`，而 `pendings_` 却不用加锁？

---

## 动手练习

理论学习完成后，建议按以下顺序做练习：

### 练习 1：编译运行（10 分钟）
```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
cd build

./echo_server               # 默认 echo server（端口 8080）
# 另开终端：echo "hello" | nc localhost 8080

./http_server 8080 ../www   # HTTP server
# 浏览器访问 http://localhost:8080
./http_server 8080 ../www 4 # 4 线程 HTTP server

./rpc_echo_server 9000      # RPC echo server（JSON payload）
# 另开终端：./rpc_echo_client 9000 "hello"
```

### 练习 2：加日志（20 分钟）
在 TcpConnection 的关键生命周期方法中加 `printf` 日志（`connectEstablished`、`connectDestroyed`、`handleRead`、`handleWrite`、`handleClose`），观察：
- 一个 TCP 连接的生命周期
- 多线程模式下，不同连接的 I/O 被分配到哪个线程

### 练习 3：实现一个简单服务（30 分钟）
替代 EchoServer，实现一个 **大写转换服务**：把收到的所有文本转成大写再发回。

### 练习 4：实现一个简单服务（60 分钟）
参考 HttpServer，实现一个 **daytime 服务**（RFC 867）：客户端连接后，服务器直接发送当前时间字符串然后关闭连接。不需要接收任何数据。

### 练习 5：添加连接统计（30 分钟）
在 TcpServer 中添加当前活跃连接数统计，并在 HTTP 响应中显示这个数字。需要处理多线程环境下的原子计数（使用 `std::atomic<int>`）。

### 进阶练习（选做）

### 练习 6：实现 HTTP/1.1 Keep-Alive ✅（主代码已实现）
主代码已支持 HTTP/1.1 keep-alive：响应行用 HTTP/1.1、`HttpResponse(false)` 不断连、`onMessage` 循环处理同一连接 Buffer 里的多个请求。想练手的话，可以自己回退到短连接再重写一遍（把 `false` 改回 `true` 并加回 `shutdown()`），或者直接做练习 7（空闲超时），补上 keep-alive 目前缺的「死连接踢掉」。

### 练习 7：实现定时断开空闲连接
使用 TimerQueue 给每个连接设置一个空闲超时（比如 30 秒）。如果连接在超时内没有收到任何数据，主动关闭连接。每次收到数据时重置定时器。

### 练习 8：实现基于 Token 的简单速率限制
给每个客户端 IP 限制每秒最多 N 个请求。超过限制则返回 HTTP 429 Too Many Requests。

### 练习 9：给 RPC Echo 增加一个方法（30 分钟）
在 `rpc_echo_server.cpp` 注册 `Echo.Upper`，把请求 JSON 里的 `message` 转成大写后返回。客户端分别调用 `Echo.Echo` 与 `Echo.Upper`，观察 `request_id` 如何把交错返回的响应配回各自的调用。

### 练习 10：实现第二个 Serializer（60 分钟）
实现一个 `LineSerializer`（id 自己选一个未使用的值，比如 `2`）：把 `JsonValue` 字符串编码成「一行文本 + `\n`」。它只处理字符串值，用来体会 `Serializer` 接口如何与帧层解耦。然后在客户端/服务端 `setSerializerId`，验证不匹配时的失败路径。

---

## 代码阅读技巧

1. **先看头文件，再看实现**：头文件定义了接口和数据结构，通常比实现更容易理解。重点关注：
   - 类继承关系
   - 成员变量的含义（善用注释）
   - 公开接口的设计

2. **追踪一个连接的完整生命周期**：这是理解整个架构最有效的方法。
   ```
   main() → TcpServer::start()
         → Acceptor::listen()
         → [客户端连接到达]
         → Acceptor::handleRead() → newConnectionCallback_
         → TcpServer::newConnection()
         → TcpConnection 构造 → connectEstablished()
         → [数据收发...]
         → TcpConnection::handleClose()
         → TcpServer::removeConnection()
         → TcpConnection::connectDestroyed()
         → TcpConnection 析构
   ```

3. **用 GDB / 断点走一遍**：在关键路径打上断点，用 `nc` 或 `curl` 触发，单步跟踪调用栈。

4. **对比 muduo 源码**：本项目是 muduo 的简化版。当你有疑问时，可以查阅 [muduo 源码](https://github.com/chenshuo/muduo) 对比实现。muduo 有完整的日志、线程池、连接器（Connector，用于客户端主动连接）等特性——这些在本项目中都被省略了。

---

## 本项目省略了什么？（和 muduo 的差距）

了解这些 gap 有助于你在读完本项目后继续深入：

| muduo 有，本项目没有 | 说明 |
|---------------------|------|
| **Logger 日志系统** | muduo 有完整的异步日志库 |
| **线程池** | 固定大小的通用线程池；本项目只有 one-loop-per-thread |
| **定时器队列优化** | muduo 用 4 个桶的 timing wheel 替代了简单最小堆 |
| **Buffer 设计** | muduo 的 Buffer 更复杂（支持 prepend 动态空间） |
| **EPOLLET 边缘触发** | 本项目只用水平触发（LT），muduo 支持 ET |
| **protobuf RPC** | 本项目提供手写 JSON 与 raw 两种 Serializer；protobuf 只是 ADR-002 中的可选插件方向 |
| **完整的 IDL / codegen** | 类型化注册与 stub 是 #44–#48 的路线图，当前注册 API 仍是「名字 + 字节」 |

---

## 常见问题 FAQ

**Q: 为什么只在 Linux 上能跑？macOS/Windows 不行吗？**
A: 因为直接使用了 Linux 特有的 `epoll`、`timerfd`、`eventfd`。macOS 有 `kqueue`，Windows 有 `IOCP`，API 完全不同。这是有意的简化——抽象层会增加代码复杂度。

**Q: 线程模型是什么？**
A: One loop per thread。每个 EventLoop 绑定一个线程，一个线程只跑一个 EventLoop。多线程就是多个 EventLoop 各自运行。不存在一个线程跑多个 loop 或一个 loop 跨多个线程。

**Q: 为什么 send() 要在 IO 线程执行？**
A: 因为 write() 系统调用需要在正确的 epoll 上注册 EPOLLOUT 事件。如果在其他线程直接 write，可能和 IO 线程的 epoll_wait 产生竞态，而且无法正确地管理 EPOLLOUT 的开关。解决方案是把 send 操作排队到 IO 线程。

**Q: Buffer 里的 prependable 8 字节是干什么的？**
A: 为协议头预留空间。比如要在已有数据前面加一个长度头（length-prefixed framing），可以直接在 prependable 区域写入而不需要移动数据。这 8 字节足够放下一个 64 位整数。

**Q: 为什么默认 serializer 是 raw，而不是 JSON？**
A: 兼容性。`serializer` 字节原来是保留字节，pre-#42 的发送方在那里写 `0`；把 `0` 定义为 raw，旧对端的帧就仍然合法。JSON 是 id `1`，example 通过 `setSerializerId(kSerializerJson)` 显式启用。

**Q: 两端 serializer 不一致会怎样？**
A: 服务端收到不认识的 id 回 `UNSUPPORTED_SERIALIZER`（`0x0D`），连接保留；客户端收到与请求不一致的响应 id 则关闭连接，让所有在途调用失败。两种都是「明确失败」，不会把错误格式的 payload 当成功返回。

**Q: 为什么 JSON 解析器要限制深度？**
A: 递归下降解析器每进入一层数组/对象就消耗栈帧。没有上限时，一个 `[[[[[...` 请求就能打爆线程栈。默认上限 64，并且在递归**之前**检查。

**Q: 为什么不用 Boost.Asio？**
A: 这是一个**教学项目**。使用 Boost.Asio 就看不到底层实现了。手写 epoll 让你真正理解 Reactor 模式。

---

## 延伸阅读

1. **[muduo 源码](https://github.com/chenshuo/muduo)** — 本项目所有代码的"完整版"。建议读完本项目后对比阅读。
2. **《Linux 多线程服务端编程》** — 陈硕（muduo 作者）的书，详细讲解了 muduo 的设计理念和 C++ 网络编程最佳实践。
3. **《UNIX 网络编程 卷一》** — 网络编程的经典教材（Stevens），第 6 章讲了 I/O 多路复用（select/poll），但未涵盖 epoll。适合补充 socket 基础知识。
4. **`man epoll`** — epoll 的官方手册，`man 7 epoll` 有详细的边缘触发/水平触发说明。
5. **[The C10K Problem](http://www.kegel.com/c10k.html)** — 经典文章，解释了为什么需要非阻塞 I/O 和事件驱动模型来处理高并发。

---

## 学习进度自查

读完一个模块后，尝试回答以下问题。如果都能回答，说明你掌握了：

### 第 2 层（事件循环）— 必须掌握 ✅
- [ ] Channel、Poller、EventLoop 三者是什么关系？
- [ ] `EventLoop::loop()` 的主循环分哪三步？
- [ ] 跨线程调用 `queueInLoop()` 时，线程安全如何保证？
- [ ] eventfd 在 EventLoop 中扮演什么角色？

### 第 4 层（TCP 层）— 必须掌握 ✅
- [ ] 一个 TCP 连接从建立到关闭的完整状态转移是怎样的？
- [ ] `send()` 的线程安全是如何实现的？
- [ ] TcpServer 如何把新连接分发给工作线程？
- [ ] 关闭一个连接时，为什么清理要回到 baseLoop 执行？

### 第 6 层（RPC）— 建议掌握
- [ ] `FrameReader`、`Codec`、`Serializer` 三者的边界分别在哪里？
- [ ] `serializer` 为什么放在帧头？响应为什么要回显它？
- [ ] `JsonValue` 为什么把整数和浮点分开存？对象为什么用 `vector` 而不是 `map`？
- [ ] 客户端超时后，迟到的响应如何被识别并丢弃？
- [ ] 服务端 handler 抛异常时，连接为什么还能继续使用？

### 综合理解
- [ ] 画出多 Reactor 模式下一帧数据的完整流向（从网卡到应用层回调）
- [ ] 画出一次 JSON RPC 调用从 `RpcClient::Call()` 到 handler 返回的完整路径
- [ ] 如果让你用其他语言（Rust/Go/Python）实现同样的架构，你会怎么设计？

---

祝学习愉快！🎉
