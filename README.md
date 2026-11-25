# ReactorNet

[![CI](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml/badge.svg?branch=reactor)](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml)

A minimalist C++ network library implementing the Reactor pattern, inspired by [muduo](https://github.com/chenshuo/muduo). No third-party dependencies — pure Linux system calls and the C++17 standard library.

> The RPC layer built on top of this transport lives on the [`extend-rpc`](https://github.com/syloe1/ReactorNet/tree/extend-rpc) branch and is not part of this branch.

## Features

- **Epoll Event-Driven** — High-performance I/O multiplexing via `epoll`
- **Timer Heap** — Min-heap timer management using `timerfd_create` for kernel-level precision
- **Read/Write Buffers** — Efficient `readv`-based input with automatic growth, handles TCP framing
- **Single Reactor / Multi-Reactor** — One-loop-per-thread architecture with round-robin load distribution
- **HTTP/1.1 Keep-Alive Server** — Static file serving with URL decoding and path traversal protection
- **Zero Dependencies** — C++17, Linux system calls only

## Architecture

```
TcpServer(baseLoop主线程)
 └── Acceptor：监听socket，只跑在baseLoop，负责accept新连接
     └── 收到新连接 → round‑robin轮询，分派给worker线程的EventLoop
          ├── worker‑0 EventLoop + EPollPoller + TcpConnection
          ├── worker‑1 EventLoop + EPollPoller + TcpConnection
          └── worker‑2 EventLoop + EPollPoller + TcpConnection
```

- **baseLoop（main 线程）**：只做`accept`，不处理业务 IO。
- **worker EventLoop**：每个线程一个 epoll 实例，负责已连接 socket 的读写事件。
**连接一旦分派到某个 worker，该连接所有 IO 事件永远在这个 worker 线程执行**，业务回调都跑在 IO 线程，不用加锁。

### Transport layer

Application code (for example the HTTP server) consumes a connection as a
`TransportPtr`, a transport-agnostic byte stream:

```
Application (e.g. the HTTP server)
        │  TransportPtr — one reliable, ordered, bidirectional byte stream
        ▼
Transport (pure virtual interface, include/Transport.h)
        ▲
        │ implemented by
TcpConnection ── Socket / Channel ── EPollPoller
        ▲
        │ created by
TcpServer / Acceptor (baseLoop)
```

A TCP connection carries exactly one stream (`streamId()` is
`kDefaultStreamId`); a QUIC connection would carry one `Transport` per stream.
**TCP is implemented today; QUIC is a future target and is not implemented.**
See [RFC-0003](docs/rfc/0003-transport-abstraction.md) for the interface,
ownership model and thread contract.

```
┌───────────────────────────────────────────┐
│  TcpServer (baseLoop)                     │
│  ┌─────────┐                              │
│  │Acceptor │──► newConnection()           │
│  └─────────┘     │                        │
│                  │ round-robin             │
│     ┌────────────┼────────────┐           │
│     ▼            ▼            ▼           │
│  EventLoop   EventLoop   EventLoop  ...   │
│  (worker 0)  (worker 1)  (worker 2)       │
│     │            │            │            │
│  TcpConn     TcpConn     TcpConn          │
│  (Transport) (Transport) (Transport)      │
└───────────────────────────────────────────┘
```

### Module Overview

| Module | Description |
|--------|-------------|
| `EventLoop` | Per-thread event loop with cross-thread task dispatch via `eventfd` |
| `EPollPoller` | Epoll-based I/O multiplexing (implements `Poller`) |
| `Channel` | Event dispatcher: binds fd + callbacks, does not own the fd |
| `TimerQueue` | Min-heap timer management using `timerfd` |
| `Transport` | Transport-agnostic byte-stream interface. TCP is implemented via `TcpConnection`; QUIC is a future target, **not implemented** ([RFC-0003](docs/rfc/0003-transport-abstraction.md)) |
| `TcpConnection` | TCP connection with input/output buffers, `shared_ptr` lifecycle; implements `Transport` |
| `TcpServer` | Multi-reactor server with accept loop and connection management |
| `TcpClient` | Client entry point: `Connector` handshake + `TcpConnection` lifecycle |
| `Acceptor` | Listening socket handler, runs in base reactor |
| `Buffer` | Non-contiguous read/write buffer with prependable space |
| `Socket` | RAII wrapper around socket fd |
| `InetAddress` | `sockaddr_in` wrapper |
| `HttpRequest` | Simple HTTP 1.0 request parser (GET only) |
| `HttpResponse` | HTTP/1.1 response builder |

## Build & Run

```bash
# Library + examples
cmake -B build
cmake --build build -j"$(nproc)"

# Library + examples + GoogleTest suites (dev-only dependency)
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

All examples are run from the `build/` directory.

### Sanitizers

ASan/UBSan and TSan cannot be enabled in the same binary, so each gets its own
build tree:

```bash
cmake -B build-asan -DBUILD_TESTING=ON -DSANITIZER=address
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure

cmake -B build-tsan -DBUILD_TESTING=ON -DSANITIZER=thread
cmake --build build-tsan -j"$(nproc)"
ctest --test-dir build-tsan --output-on-failure
```

Both run on every pull request as the `Sanitizers` CI jobs.

### Git blame

`.git-blame-ignore-revs` excludes the mechanical namespace refactor from
`git blame`. Enable it locally with:

```bash
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

GitHub applies the file automatically.

### Echo Server

```bash
# Usage: ./echo_server [numThreads]

# Single-threaded (default)
./echo_server

# 4 worker threads
./echo_server 4

# Test with netcat
echo "Hello, Reactor!" | nc localhost 8080
```

### HTTP Server

```bash
# Usage: ./http_server <port> <docRoot> [numThreads]
# docRoot must be given explicitly — no guessing of the working directory.

# Single-threaded HTTP
./http_server 8080 ../www

# 4 worker threads
./http_server 8080 ../www 4

# Test with curl
curl http://localhost:8080/
```

## API Example

All public types live in `namespace reactornet`; either add `using namespace reactornet;`
(as below) or qualify names as `reactornet::EventLoop`.

```cpp
#include "TcpServer.h"
#include "EventLoop.h"
#include "InetAddress.h"

using namespace reactornet;
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

int main() {
    EventLoop loop;
    TcpServer server(&loop, InetAddress(8080));

    server.setMessageCallback([](const TcpConnectionPtr& conn,
                                  Buffer* buf, Timestamp) {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);  // Echo back
    });

    server.setThreadNum(4);  // 4 worker threads
    server.start();
    loop.loop();
}
```

## Requirements

- Linux kernel 2.6.27+ (for `eventfd`, `timerfd`, `signalfd`, `epoll`)
- GCC 8+ or Clang 7+ (C++17 support)
- CMake 3.10+

## File Structure

```
ReactorNet/
├── CMakeLists.txt
├── README.md
├── Learn.md                 # Guided code-reading tour (Chinese)
├── include/                 # Core network library public headers
│   ├── EventLoop.h  Channel.h  Poller.h  EPollPoller.h
│   ├── Timer.h  TimerQueue.h  Buffer.h  EventLoopThread.h
│   ├── Socket.h  InetAddress.h  Acceptor.h  Connector.h
│   ├── TcpConnection.h  TcpServer.h  TcpClient.h  Transport.h
│   └── HttpRequest.h  HttpResponse.h
├── src/                     # Core sources -> libreactornet.a
├── example/
│   └── echo_server.cpp  http_server.cpp
├── tests/                   # Core GoogleTest suites (dev-only)
├── bench/
│   └── serialization_bench.cpp
├── docs/
│   └── rfc/                 # Transport contract
└── www/                     # Static files (HTTP server)
```

## Documentation

| Document | What it answers |
|----------|-----------------|
| [Learn.md](Learn.md) | Guided reading order: which file to open, why it exists, what to try |
| [RFC-0003](docs/rfc/0003-transport-abstraction.md) | The `Transport` interface and its thread contract |

## License

MIT — see [LICENSE](LICENSE).
