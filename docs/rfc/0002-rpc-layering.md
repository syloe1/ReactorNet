# RFC-002：RPC 分层、模块边界与线程模型 v0.1

| 项目 | 内容 |
|---|---|
| RFC 编号 | 0002 |
| 标题 | RPC 分层、模块边界与线程模型 |
| 状态 | Draft（待 review） |
| 日期 | 2026-10-17 |
| 里程碑 | RPC M0 · 设计 |
| 对应 issue | #16 |
| 依赖 | RFC-0001（线格式）、RFC-0003（Transport） |
| 被依赖 | #27 #31 #32 #36 #39 |
| 接口版本 | 不适用（本文档不定义 API） |

本文档定死 RPC 层与网络层的边界：目录与构建目标、依赖方向、线程模型、错误分类。

它**不定义任何 API 签名**，也**不决定序列化**。前者是 #29–#32 的事，后者是 #41 的事。本文档只回答一个问题的边界版本：**什么代码应该放在哪一层，以及它允许触碰哪些线程与状态。**

---

## 1. 摘要

ReactorNet 的 RPC 层是网络层的一个**消费者**，不是网络层的一部分：

```
用户代码 / example
        │
        ▼
rpc/                 RpcServer · RpcClient · Codec · ServiceRegistry
        │            构建目标：libreactornet_rpc.a
        ▼
include/Transport.h  TransportPtr：一条可靠有序的字节流（RFC-0003）
        │
        ▼
include/ + src/      TcpServer · TcpConnection · Connector · TcpClient
                     EventLoop · Channel · Poller · TimerQueue · Buffer
                    构建目标：libreactornet.a（零第三方依赖）
        │
        ▼
Linux：epoll · timerfd · eventfd · socket
```

四条结论：

1. **依赖方向单向**：`rpc/` 可以 include 核心；核心永远不 include `rpc/`（CI grep 强制）。
2. **网络层不认识 RPC**：核心库里不出现 RPC 的类型名、字段名与词汇（CI grep + 一条 review 判断句）。
3. **线程状态收敛**：客户端的 `nextId_` / `pendings_` 只属于它自己的 loop 线程；服务端注册表在 `start()` 后只读，因此多 IO 线程并发查表不需要锁。
4. **错误分三类**：传输错误没有帧，协议错误「回一帧然后关连接」，业务错误「帧完好、连接可继续用」。三类在 RFC-0001 的 `status` 上有各自的落点。

---

## 2. 为什么先写这份文档

没有这条边界，RPC 的需求会一点点渗进 `Channel`、`EventLoop`、`TcpConnection`：为了拿超时，给 `TcpConnection` 加一个 `deadline` 字段；为了配对请求，给 `Buffer` 加一个 `request_id`；为了序列化，让 `TcpServer` 认识 JSON。每一步都「只是加一个字段」，最后网络库变成一个只有 RPC 能用的东西。

这份文档的验收标准与 RFC-0001 / RFC-0003 相同：**另一个人只看这份文档，就能判断一个改动属于哪一层、该用哪个构建目标、允许在哪个线程执行。**

与 RFC-0001 的分工：RFC-0001 管「字节怎么摆」，本文档管「代码放哪里、跑在哪个线程、错误怎么分类」。与 RFC-0003 的分工：RFC-0003 管「连接在应用眼里长什么样」，本文档管「RPC 如何使用这条连接」。

---

## 3. 术语与约定

| 术语 | 含义 |
|---|---|
| 核心库（core） | 构建目标 `reactornet`，源码在 `include/` 与 `src/` |
| RPC 库（rpc） | 构建目标 `reactornet_rpc`，源码在 `rpc/` |
| IO 线程 | 拥有一个 `EventLoop` 的线程；该 loop 上的所有连接与定时器都归它 |
| loop 线程 | 某个 `EventLoop` 所属线程；对 `RpcClient` 而言就是它自持的那个线程 |
| 业务线程 | 调用 RPC 的线程，可以是任意线程，包括主线程；**不是** loop 线程 |
| handler | 服务端注册的业务方法实现 |
| pending call | 已发出、尚未收到响应或超时的客户端调用记录 |
| 冻结（freeze） | 注册表从可变进入只读的那一刻，见 §7.3 |

约定：`MUST` / `MUST NOT` / `SHOULD` / `MAY` 按 RFC 2119 解释。

「必须在 loop 线程」指必须在该对象 `getLoop()` 所属线程执行——这是 one-loop-per-thread 模型的直接推论，与 RFC-0003 §9.2 一致。

---

## 4. 分层与职责

### 4.1 每层负责什么

| 层 | 负责 | 不负责 |
|---|---|---|
| 用户代码 / example | 业务语义、调用时机 | 分帧、连接生命周期、线程调度 |
| `rpc/` | 帧编解码、请求配对、服务注册与分发、超时与错误映射、客户端`Call` | 直接读写 fd、epoll 注册、accept/connect 状态机 |
| `Transport` | 一条可靠有序双向字节流的抽象（RFC-0003） | 帧边界、请求语义、序列化 |
| 核心网络层 | socket 生命周期、事件循环、定时器、缓冲、accept/connect | 任何 RPC 概念 |
| Linux | epoll / timerfd / eventfd / socket | —— |

`rpc/` **可以使用**核心层的全部公开设施：`TcpServer` 做监听、`Connector`/`TcpClient` 做客户端连接、`EventLoop::runAfter` 做超时、`Buffer` 做收发缓冲、`TransportPtr` 做会话接口。这些设施都必须先作为「网络库本来就该有的东西」独立存在（§6.5）。

### 4.2 依赖方向

```
rpc/ ──────────────► include/ + src/ ──────────────► Linux
  │                        │
  └──► 第三方（可选，仅 rpc 目标）        （核心库为零依赖）
```

反向的箭头**一条都不允许**。落地检查见 §6。

---

## 5. 目录与构建目标

### 5.1 目录

```
rpc/
├── include/        # RPC 公开头文件（RpcServer.h / RpcClient.h / RpcStatus.h ...）
├── src/            # RPC 实现 → libreactornet_rpc.a
└── tests/          # RPC 测试（仅在 BUILD_TESTING 下参与构建）
```

不把 RPC 头文件放进顶层 `include/`：那个目录的语义是「核心库的公开接口」，放进去会让「核心库零 RPC 概念」这条边界在目录结构上就自相矛盾。

### 5.2 CMake 目标

```cmake
# rpc/CMakeLists.txt（由顶层 add_subdirectory(rpc) 引入）
add_library(reactornet_rpc STATIC ${RPC_SOURCES})
target_include_directories(reactornet_rpc PUBLIC ${CMAKE_SOURCE_DIR}/rpc/include)
target_link_libraries(reactornet_rpc PUBLIC reactornet)
```

约束：

- `reactornet_rpc` **MUST** 链接 `reactornet`；`reactornet` **MUST NOT** 链接 `reactornet_rpc`。
- 核心目标 `reactornet` 的 `LINK_LIBRARIES` **MUST** 只有 `Threads::Threads`（C++ 标准库不出现，Linux 系统调用不产生链接项）。
- 第三方序列化库（protobuf / JSON 库等）**MUST** 只出现在 `rpc/CMakeLists.txt`，且只挂到 `reactornet_rpc`。由于依赖方向是单向的，它们不会传播进核心库。
- `example/` 里的 RPC 示例链接 `reactornet_rpc`；纯网络示例只链接 `reactornet`。

配置期断言（在 `rpc/CMakeLists.txt` 内，早于任何编译）：

```cmake
get_target_property(_core_links reactornet LINK_LIBRARIES)
if(_core_links MATCHES "reactornet_rpc")
    message(FATAL_ERROR "reactornet (core) must not link reactornet_rpc")
endif()
```

---

## 6. 五条边界规则

每条规则要么是一条可执行的 CI 检查，要么是一句可在 review 时直接使用的判断句。**不允许出现「大致上遵守」这种状态。**

### 6.1 规则一：依赖单向

`rpc/` 可以 include `include/`；`include/` 与 `src/` 不得 include `rpc/`。

CI 检查：

```bash
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"](reactornet/)?rpc/' -- include src; then
  echo 'boundary violation: core includes rpc/' >&2
  exit 1
fi
```

CMake 侧的对应检查见 §5.2。两条都要有：grep 抓源码，CMake 断言抓链接。

### 6.2 规则二：网络层不认识 RPC

核心库里不得出现 RPC 的类型名、字段名与词汇。CI 检查的**可执行部分**只覆盖无歧义的标识符：

```bash
# RPC 类型 / 字段 / 概念标识符；核心库中一次都不能出现
if git grep -nwE 'Rpc[A-Za-z]+|StatusCode|ServiceRegistry|PendingCall|AsyncCall|request_id' -- include src; then
  echo 'boundary violation: RPC identifier in core' >&2
  exit 1
fi

# 服务名 / 方法名相关的 RPC 专有拼写
if git grep -niwE 'svc(_len|_name)?|service(_name|_id)?' -- include src; then
  echo 'boundary violation: RPC vocabulary in core' >&2
  exit 1
fi
```

**为什么 bare `service` / `method` 不进 grep**：核心库里的 `HttpRequest::method()` 是 HTTP 方法，是合法且必要的网络词汇。把 `method` 加进黑名单只会制造一条永远红的检查，然后被人用 allowlist 绕过——那比没有检查更糟。裸词 `service` 目前为零命中，但同样可能在未来被非 RPC 概念使用；因此只把**RPC 专有拼写**（`svc_len`、`service_name` 等）放进 grep。

对裸 `service` / `method` 的语义泄漏，用**判断句**兜底：

> **这个改动放到一个纯 TCP 库上是否依然成立？**
>
> 如果它只有在「对端是一个 RPC 客户端/服务端」的前提下才有意义，它就不属于核心库。

例：给 `Channel` 加 `onRequest()` 不成立，因为纯 TCP 没有「请求」；给 `EventLoop` 加 `runAfter()` 成立，因为任何网络程序都需要定时器——尽管它最早的需求来自 RPC 超时。

### 6.3 规则三：核心库零依赖

核心库只允许 C++ 标准库与 Linux 系统调用。CI 检查：

```bash
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"](google/protobuf|protobuf|nlohmann|jsoncpp|grpc|boost)/' -- include src; then
  echo 'boundary violation: third-party include in core' >&2
  exit 1
fi
```

`reactornet` 的 `LINK_LIBRARIES` 断言见 §5.2。GoogleTest 只进测试目标，不进任何库目标——这一点顶层 `CMakeLists.txt` 的注释已经写明，本文档将其提升为边界规则。

### 6.4 规则四：不为 RPC 改事件语义

以下语义**一条都不改**（即使 RPC 用起来更方便）：

| 设施 | 冻结的语义 |
|---|---|
| `Channel` | 回调时机、回调线程、事件到回调的映射 |
| `Poller` | `poll()` 返回值语义、事件填充方式 |
| `Buffer` | `readerIndex_` / `writerIndex_` 的读写指针规则、扩容策略 |
| `EventLoop` | `runInLoop` / `queueInLoop` 的执行时机与线程保证 |
| `TimerQueue` | 定时器所有权、取消语义、回调线程 |
| `Transport` | RFC-0003 §9.2 的线程契约 |

如果 RPC 真的需要另一种语义，做法是**在核心库新增一个能力**（例如 #20 的 `TimerId`、#21 的 `runAfter`），并作为独立的网络层 issue 带自己的测试合入；不是把 RPC 的特殊要求塞进已有语义。

Review 判断句：

> **这个改动是否改变了任何已有调用方对时序/线程/所有权的一次观察？**
>
> 是 → 它是网络层语义变更，必须独立成 issue，不能夹在 RPC PR 里。

### 6.5 规则五：每个网络层改动都要能独立讲故事

核心库的每一次改动，都必须能用**不出现「RPC」两个字**的句子解释清楚它为什么存在：

| 改动 | 独立故事 |
|---|---|
| `Connector`（#24） | 一个 TCP 网络库本来就该能主动发起连接 |
| `TcpClient`（#25） | 有了 Connector 之后，客户端连接也需要和 TcpServer 对称的生命周期管理 |
| `TimerId`（#20） | 任何定时器 API 都需要一个不会悬垂的取消句柄 |
| `EventLoop::runAfter`（#21） | 任何事件循环都需要注册延迟任务 |
| `TcpConnection` 发送路径修复（#23） | 跨线程 `send` 的生命周期与内联写是库自身的正确性问题 |

反过来，如果一句话里必须出现「因为 RPC 要……」，那这个改动就应该放进 `rpc/`。

### 6.6 CI 中的落地位置

上述 grep 作为 `ci.yml` 的一个独立 step，与构建、测试并列：

```yaml
- name: Architecture boundaries
  run: ./scripts/check-boundaries.sh
```

脚本内容就是 §6.1–§6.3 的命令。**它必须能失败**：故意在核心头文件里写一个 `RpcRequest`，CI 要变红；这与 #26 对 sanitizer 作业的要求是同一条纪律。

---

## 7. 线程模型

### 7.1 总则

ReactorNet 是 one-loop-per-thread。由此推出三条铁律：

1. **一个连接的所有 IO 与状态只属于它的 IO 线程。** 跨线程只能投递任务，不能直接改状态。
2. **loop 线程不允许阻塞。** 阻塞一个 loop 会同时停掉挂在它上面的所有连接、所有定时器。
3. **共享只读数据可以无锁。** 可变共享状态要么收敛到单线程，要么显式加锁——本项目优先前者。

### 7.2 客户端：`RpcClient` 自持一个 `EventLoopThread`

```
业务线程                    RpcClient 的 loop 线程
   │                              │
   │ Call(req)                    │
   ├──── runInLoop ──────────────►│
   │                              │ nextId_++ → pendings_[id] = call
   │                              │ encode → Transport::send
   │                              │ runAfter(deadline) → 超时定时器
   │                              │
   │                              │ FrameReader 解出 RESPONSE
   │                              │ pendings_.find(id) → erase
   │                              │ cancelTimer → 完成 future / 回调
   │◄──── future / callback ──────┤
```

| 状态 | 归属 | 访问规则 |
|---|---|---|
| `nextId_` | 客户端 loop 线程 | 只在该线程自增；不在构造线程、不在业务线程触碰 |
| `pendings_` | 客户端 loop 线程 | 只在该线程插入/查找/删除；业务线程不得直接读写 |
| 超时定时器 | 客户端 loop 线程 | 由 loop 线程注册与取消（#38） |
| 连接对象 `TransportPtr` | 客户端 loop 线程 | 生命周期由客户端持有；断开时置空 |
| 用户的响应回调 | 客户端 loop 线程 | 回调里可以做轻量工作；不得阻塞 |
| 调用方拿到的 future | 业务线程 | 业务线程可以等待；**loop 线程不得等待** |

**客户端线程契约（MUST）：**

- `Call()` 在业务线程调用时，可以把请求投递进 loop 线程，并在**自己的线程**上等待结果或 deadline。
- `Call()` 在客户端 loop 线程调用时，**MUST NOT 阻塞等待响应**。响应只能由同一个 loop 线程处理，阻塞等待就是死锁。同步形态在 loop 线程上 MUST 立即失败（断言或返回错误），异步形态（#35）不受此限。
- 任何注册到 `RpcClient` 的回调都跑在 loop 线程；回调里 MUST NOT 调用同步 `Call()`，理由同上。
- `connect()` / `disconnect()` 可以从任意线程调用，但内部 MUST 经 `runInLoop` 投递；连接状态只在 loop 线程读写。
- 连接断开时，所有 pending call MUST 在 loop 线程上被批量失败，不能留下永远不完成的 future。

「在 loop 线程上做同步等待」是这套模型里最容易犯的错，也是面试里最值得讲的一条：**不是所有阻塞都错，阻塞 loop 线程才错。**

### 7.3 服务端：handler 跑在连接所属的 IO 线程

```
baseLoop（accept）
   │ 新连接分派给某个 worker
   ▼
worker loop ──► FrameReader ──► Dispatcher ──► handler
   ▲                                            │
   └────────────── response encode ◄────────────┘
```

| 状态 | 归属 | 访问规则 |
|---|---|---|
| 监听 socket / accept | baseLoop | 只在 baseLoop 线程 |
| 连接状态、输入/输出缓冲 | 连接所属 IO 线程 | 只在该线程读写 |
| 帧解码状态 | 连接所属 IO 线程 | 每个连接一份，天然无共享 |
| 服务注册表 | 冻结后只读 | `start()` 之前注册完毕；之后任何线程只读 |
| 业务 handler | 连接所属 IO 线程 | 默认直接在 IO 线程执行 |
| 用户 service 对象的可变状态 | 用户负责 | 多 IO 线程可能并发调用同一对象，用户必须自己保证线程安全 |

**注册表的冻结与发布：**

1. `registerService` / `registerMethod` 只允许在 `start()` 之前调用。`start()` 之后调用 MUST 失败（断言或返回错误），不得静默接受。
2. `start()` 是冻结点。冻结发生在 worker 线程创建之前（或至少发生在任何连接被分派之前），因此线程创建提供了 happens-before：worker 看到的注册表内容一定是完整的。
3. 冻结后注册表只读，所以任意 IO 线程并发 `find` **不需要锁**。
4. 这条规则的正确性依赖「不再写」。若将来要支持运行期动态注册，MUST 先改本文档，并明确引入的同步方案——不能悄悄加一个 mutex 就当成没发生过。

**handler 的线程纪律：**

- handler 默认在连接所属 IO 线程执行，**MUST NOT 阻塞**。
- 需要阻塞的工作（磁盘、数据库、同步 HTTP）MUST 交给外部工作线程；完成后 MUST 把结果投递回连接所属 loop（`runInLoop`）再发送响应。
- handler 内 MUST NOT 直接触碰连接缓冲、pending 表或其他连接的状态；它只能使用自己的入参，并通过返回/抛出的结果交给分发层。
- handler 抛出的异常 MUST 在分发边界被捕获并映射为 `INTERNAL_ERROR`（§8），**绝不允许异常穿过 `EventLoop` 的调用栈**。

### 7.4 「在 handler 里 sleep 会发生什么」——明确答案

假设 handler 里 `sleep(1)`：

1. 该连接所在 IO 线程被占住 1 秒；
2. 同一个 loop 上的**所有其他连接**在这 1 秒内收不到读事件、写事件，已经可读的数据要等 sleep 结束才被处理；
3. 同一个 loop 上的**所有定时器**（包括 RPC deadline、心跳、连接超时）全部延迟触发；
4. 如果服务端只有一个 IO 线程（`setThreadNum(0)` 或 1），整个服务端停顿 1 秒；
5. 如果有 N 个 worker，只有 1/N 的连接受影响——但受害的那批连接会同时超时，表现为「一批请求一起失败」，很难排查。

更糟的是：**服务端无法抢占正在运行的 handler**。deadline 定时器也在同一个 loop 上，handler 不返回，定时器就没有机会触发。因此「handler 执行超时」在单 loop 内不可实现；M8 的线程池也不是为了给 handler 加超时，而是为了不阻塞 loop。

结论：handler 是「短、非阻塞、不共享可变状态」。在 M8 落地之前，任何可能超过毫秒级的操作都不应该直接写在 handler 里。

### 7.5 跨线程发送

- `Transport::send()` 是 RFC-0003 明确允许跨线程调用的两个方法之一（另一个是 `forceClose()`），内部 `runInLoop` 回本线程。
- 但「线程安全的 `send`」不等于「可以在任意线程编排连接生命周期」。工作线程完成计算后，SHOULD 把整个完成动作（发送 + 可能的关闭）投递回连接 loop，而不是在工作线程里分别调用 `send` 和 `forceClose`——两次调用之间连接可能已经断开。
- 服务端的响应发送尤其如此：响应必须与「这条连接仍然有效」的判断在同一线程、同一顺序里完成。

### 7.6 线程归属速查表

| 动作 / 状态 | 允许的线程 | 说明 |
|---|---|---|
| `RpcClient::Call`（同步） | 业务线程 | loop 线程上 MUST 立即失败 |
| `RpcClient::Call`（异步） | 任意线程 | 内部投递到 loop |
| `nextId_` / `pendings_` 读写 | 客户端 loop | 唯一所有者 |
| 客户端响应回调 | 客户端 loop | 用户回调也在该线程 |
| `RpcServer` 注册方法 | `start()` 之前的任意单线程 | 冻结后只读 |
| 注册表查找 | 任意 IO 线程 | 冻结后只读，无需锁 |
| handler | 连接 IO 线程 | 不阻塞、不碰共享可变状态 |
| 连接 buffer / 解码状态 | 连接 IO 线程 | 不跨线程 |
| `Transport::send` / `forceClose` | 任意线程 | 实现内部回本线程 |
| 其他 `Transport` 方法 | 连接 IO 线程 | RFC-0003 §9.2 |
| accept / listen socket | baseLoop | 不跨线程 |
| 定时器注册 / 取消 | 所属 loop 线程 | 跨线程用 `runInLoop` 投递 |

---

## 8. 错误模型

### 8.1 三类错误

| 类别 | 定义 | 第一观察者 | 有没有合法响应帧 | 连接还能用吗 |
|---|---|---|---|---|
| **传输错误**（transport） | 连接建立失败、连接断开、读写失败、客户端 deadline 到期 | 内核 / `Transport` | 没有 | 不能（deadline 除外） |
| **协议错误**（protocol） | 对端违反了 RFC-0001 的线格式 | 帧解码器 | 有（部分情况故意不回） | 不能，回完即关 |
| **业务错误**（application） | 帧合法，但请求无法完成：未知服务/方法、handler 失败、应用自定义错误 | 注册表 / dispatcher / handler | 有 | 能，连接继续复用 |

**分类依据不是「谁的责任」，而是「连接状态是否仍然可信」。** 协议错误意味着字节流已经不可信，唯一安全动作是关闭；业务错误意味着分帧完好、只是这次调用失败，连接可以继续。

### 8.2 三类错误在 RFC-0001 协议里的落点

| 错误 | 协议落点 | 连接动作 | 备注 |
|---|---|---|---|
| 连接被拒绝 / 连接超时 | 没有帧（还没建立连接） | 无连接可关 | 客户端本地错误 |
| 对端关闭 / `ECONNRESET` / 写失败 | 没有帧 | 连接已失效 | 所有 pending call 一起失败 |
| 客户端 deadline 到期 | 没有帧；v0.1 没有 cancel 消息 | **保留连接** | 迟到的响应按 RFC-0001 §8.7 丢弃 |
| `magic` 不匹配 | **不回响应** | 立即关闭 | RFC-0001 §7 第 1 条 |
| 方向不符（client 收到 REQUEST / server 收到 RESPONSE） | 不回响应 | 关闭 | RFC-0001 §7 第 8 条 |
| `version` 未知 | `UNSUPPORTED_VERSION`（`0x01`） | 回后关闭 | |
| `msg_type` 未知 | `UNSUPPORTED_MSG_TYPE`（`0x02`） | 回后关闭 | |
| 已定义未实现的 `flags` 位 | `UNSUPPORTED_FLAG`（`0x03`） | 回后关闭 | |
| `request_id == 0` | `BAD_REQUEST_ID`（`0x04`） | 回后关闭 | |
| `body_len` 超上限 | `BODY_TOO_LARGE`（`0x05`） | 回后关闭 | 校验先于分配 |
| body 长度前缀越界 / 空服务名 / 空方法名 | `MALFORMED_BODY`（`0x06`） | 回后关闭 | |
| 未知服务 | `UNKNOWN_SERVICE`（`0x07`） | **保持** | |
| 未知方法 | `UNKNOWN_METHOD`（`0x08`） | **保持** | |
| handler 抛异常 / 内部失败 | `INTERNAL_ERROR`（`0x09`） | **保持** | 异常在分发边界捕获 |
| 应用自定义失败 | `0x80`–`0xFF` | **保持** | RPC 层不解释，透传给调用方 |
| 响应 `status` 是 `0x0A`–`0x7F` 未定义值 | 对端发来的帧 | **保持** | RFC-0001 §8 第 3 条：按失败容忍 |
| RESPONSE 的 `request_id` 本地没有 pending | 合法帧 | **保持** | RFC-0001 §8 第 7 条：丢弃，不是错误 |

### 8.3 服务端的判定顺序

协议错误必须先于业务错误判定，否则一个畸形帧可能被当成合法请求送进 dispatcher：

```
读事件
  │
  ├─ 传输层失败？──────────────► 关闭连接，所有 pending（若有）失败
  │
  ├─ magic 错？────────────────► 不回帧，关闭
  ├─ version/type/flags/rid 错？► 回协议错误帧，关闭
  ├─ body 长度前缀越界？───────► 回 MALFORMED_BODY，关闭
  │
  ├─ 服务/方法不存在？─────────► 回 0x07 / 0x08，保持连接
  │
  └─ handler 执行
        ├─ 正常返回 ───────────► 回 OK
        ├─ 抛异常 ─────────────► 回 INTERNAL_ERROR，保持连接
        └─ 返回业务错误 ───────► 回 0x09 / 应用自定义，保持连接
```

顺序的依据：**在字节流被证明合法之前，不做任何业务判断。** 一个 `magic` 错误的连接可能根本不是 RN 协议，拿它的 body 去查服务表毫无意义。

### 8.4 客户端看到的错误

客户端不直接看到帧头 `status` 的原始数字，而是看到 RPC 层映射后的三类错误（具体 API 在 #39 定）：

| 客户端错误类别 | 触发 | pending call 的结局 | 是否可重试 |
|---|---|---|---|
| transport | 连接失败 / 断开 / 写失败 | 立即失败；连接关闭时全部失败 | 重连后可以尝试；非幂等调用需业务判断 |
| transport（deadline） | 本地定时器到期 | 该 call 失败，连接保留 | 业务决定；迟到响应被丢弃 |
| protocol | 收到 `0x01`–`0x06`，或本地解码失败 | 该 call 失败；连接随即关闭，其余 pending 按 transport 失败 | 同连接不可重试；重连后是否重试由业务决定 |
| application | 收到 `0x07`/`0x08`/`0x09`/`0x80`–`0xFF`、或未知 `status` | 仅该 call 失败，连接保留 | 业务决定（通常未知方法不可重试） |

**为什么 deadline 不是协议错误**：v0.1 协议没有 deadline 字段，也没有 cancel 消息。deadline 完全是客户端本地的定时器行为，对端可能仍在正常处理；把连接关掉会把「服务器慢」升级成「所有并发调用一起失败」。因此 deadline 只失败那一个 call，连接保留。

**为什么 `request_id == 0` 是协议错误而不是业务错误**：`0` 是未初始化哨兵值，出现它说明发送方有 bug，而不是「这次业务失败了」。RFC-0001 §4.6 把它列为 `0x01`–`0x06` 协议级错误，本文档沿用。

### 8.5 错误文本与错误码

- 程序**只看**帧头 `status` 分类，不看 body 里的错误文本。
- 错误文本是给人看的诊断信息，允许为空（RFC-0001 §8 第 4 条），MUST NOT 被解析成结构化数据。
- `INTERNAL_ERROR` 的文本 SHOULD 是固定的通用描述，SHOULD NOT 包含堆栈、内部路径、SQL 等敏感细节——错误模型同时是一条信息泄漏边界。

### 8.6 异常边界

- 异常 MUST NOT 穿过 `EventLoop`、`Channel` 或 `Transport` 的调用栈。
- 服务端在 dispatcher 调用 handler 的边界捕获所有异常，映射为 `INTERNAL_ERROR`。
- 客户端在用户回调边界同样捕获，决不让用户回调的异常打断响应配对（`pendings_` 的清理必须先于回调、或在 RAII 保护下完成）。
- `std::bad_alloc` 在畸形帧场景下是真实风险（RFC-0001 §4.4）；解码器 MUST 在校验长度上限之后才分配内存。这不是靠 catch 能解决的问题，必须在分配前拦住。

---

## 9. 对下游 issue 的约束

| Issue | 本文档提供的约束 |
|---|---|
| #27 帧头编解码 | 代码放 `rpc/`；常量与 RFC-0001 同源；核心库零改动 |
| #28 FrameReader | 每连接一份状态，只在连接 IO 线程访问；不依赖单次 `read` 边界 |
| #29 信封与 Codec | payload 对协议层不透明；Serializer 的决策留给 #41 |
| #30 ServiceRegistry | §7.3 的冻结与发布规则；`start()` 后只读、查找无锁 |
| #31 RpcServer | handler 在连接 IO 线程执行；§7.4 的禁止阻塞；异常边界 |
| #32 RpcClient | 自持 `EventLoopThread`；`nextId_`/`pendings_` 仅 loop 线程 |
| #34 PendingCall 表 | 只属于客户端 loop 线程；断开时批量失败 |
| #35 AsyncCall | 回调在 loop 线程；异步形态是 loop 线程上唯一允许的调用形态 |
| #36 跨线程 Call | §7.2 的同步/异步线程契约；loop 线程上同步调用 MUST 失败 |
| #37 单调时钟 | 截止时间用单调时钟；属于核心定时器语义，按 §6.4 独立成网络层改动 |
| #38 超时与 deadline | deadline 是本地行为；到期只失败该 call，不关连接 |
| #39 错误模型 | §8 的三类划分、映射表与判定顺序 |
| #40 畸形帧加固 | §8.3 的判定顺序；校验先于分配 |
| #41–#43 序列化 | 只在 `rpc/`；第三方依赖只挂 `reactornet_rpc`；核心库零依赖 |
| #44–#46 类型化注册 / Stub | 类型擦除发生在 `rpc/` 的注册边界，见 ADR-001；核心库仍不认识 RPC |
| #47 文档 | README / Learn 的 RPC 章节必须引用本 RFC 与 RFC-0001 |

**修改流程**：先改本文档，再改实现。任何跨越边界的改动如果确实必要，先提 RFC 修订，而不是在 PR 里顺手做掉。

---

## 10. 非目标（Non-Goals）

本文档**不**做以下事情：

- **不写代码。** 没有 `.h`、没有 `.cpp`、没有 CMake 改动。
- **不定义具体 API 签名。** 不规定 `RpcServer::registerService` 的参数形态，不规定 `Call` 返回 `future` 还是回调——这些是 #29–#32 的实现决定，只要满足本文档的线程与错误契约。
- **不决定序列化。** JSON / protobuf / 自研二进制由 ADR-002（#41）决定。
- **不设计服务发现、负载均衡、重试、连接池。** 这些都在 M6 之后，本文档不预留 API。
- **不引入线程池。** handler 的阻塞问题是 M8 的事；本文档只明确「现在不能阻塞」以及为什么。
- **不改变任何现有网络层语义。** 见 §6.4。
- **不定义压缩、TLS、鉴权。** 与 RFC-0001 §11 保持一致。

---

## 11. 验收自检（对应 issue #16 的 Acceptance Criteria）

| 验收标准 | 覆盖位置 |
|---|---|
| 画分层图并标注每层职责 | §1 图 + §4.1 表 |
| 每条边界规则都能翻译成可执行 grep 或 review 判断句 | §6.1 / §6.2 / §6.3 的 grep，§6.2 / §6.4 / §6.5 的判断句 |
| 线程模型表格（谁在哪个线程、哪些状态归谁） | §7.2 / §7.3 / §7.6 |
| 错误分类与它们在协议里的落点 | §8.1 / §8.2 / §8.3 |
| 「handler 里 sleep 会发生什么」有明确答案 | §7.4（五条后果 + 不可抢占的结论） |
| 客户端「不得在 loop 线程阻塞等待」写明 | §7.2 客户端线程契约 + §7.6 速查表 |
| `rpc/` 目录与 `libreactornet_rpc.a` 目标 | §5.1 / §5.2 |
| 注册表启动后不可变的发布语义 | §7.3（冻结、happens-before、无锁查找） |
| 错误分类不是拍脑袋，有判定顺序 | §8.3（协议先于业务） |

---

## 12. 开放问题

| # | 问题 | 为什么现在不定 |
|---|---|---|
| 1 | `RpcClient` 的连接断开后是否自动重连 | 重连策略（退避、幂等、在途请求处置）需要独立设计，属 M6+；v0.1 只要求「断开时所有 pending 失败」 |
| 2 | 注册表是否拥有 service 对象 | 所有权模型影响 #30 与 #45 的接口。本文档只要求冻结后只读，不规定持有方式 |
| 3 | 同一个 service 对象被多个 IO 线程并发调用 | 本文档把线程安全责任交给用户；是否改为「每线程一个实例」需要真实压测数据，属 M8 |
| 4 | 服务端如何执行「handler 超时」 | 单 loop 内无法抢占正在运行的 handler（§7.4）。真要支持，需要线程池 + 协作式取消，属 M8 |
| 5 | 同步 `Call` 是否应该完全禁止 | 本文档允许业务线程同步等待、禁止 loop 线程同步等待；是否最终只保留异步形态，等 #35/#36 的使用反馈 |
| 6 | 跨进程/多语言互通 | 协议层已独立（RFC-0001），但序列化与 IDL 未定。不在本路线图内 |

---

## 13. 参考资料

- RFC 2119 — `MUST` / `SHOULD` / `MAY` 的定义
- RFC-0001（`docs/rfc/0001-rpc-protocol.md`）— 帧格式、`status` 取值、拒收/容忍清单
- RFC-0003（`docs/rfc/0003-transport-abstraction.md`）— `Transport` 接口与线程契约
- muduo 的 `EventLoop` / `TcpConnection`：one-loop-per-thread 与跨线程投递的参考实现
- Netty 的 `EventLoop` 线程模型：channel 状态归属单个 event loop，跨线程操作必须 `execute`
- gRPC 的错误状态模型：传输错误 / 协议错误 / 应用状态码的分层方式（本文档 §8 的对照设计）
- Go `net/rpc`：服务注册与方法分发的最小形态；其 `ServeCodec` 的「每连接一个状态机」与 §7.3 一致

## 修订历史

| 版本 | 日期 | 说明 |
|---|---|---|
| v0.1 | 2026-10-17 | 初稿：分层图、`rpc/` 目录与构建目标、五条边界规则及 CI grep、客户端/服务端线程模型、三类错误模型与 RFC-0001 `status` 映射、6 条开放问题 |
