# ReactorNet

[![CI](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml)

A minimalist C++ network library implementing the Reactor pattern, inspired by [muduo](https://github.com/chenshuo/muduo). No third-party dependencies — pure Linux system calls.

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

- **baseLoop（main 线程）**：只做`accept`，不处理业务 IO。
- **worker EventLoop**：每个线程一个 epoll 实例，负责已连接 socket 的读写事件。
**连接一旦分派到某个 worker，该连接所有 IO 事件永远在这个 worker 线程执行**，业务回调都跑在 IO 线程，不用加锁。


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
└───────────────────────────────────────────┘
```

### Module Overview

| Module | Description |
|--------|-------------|
| `EventLoop` | Per-thread event loop with cross-thread task dispatch via `eventfd` |
| `EPollPoller` | Epoll-based I/O multiplexing (implements `Poller`) |
| `Channel` | Event dispatcher: binds fd + callbacks, does not own the fd |
| `TimerQueue` | Min-heap timer management using `timerfd` |
| `TcpConnection` | TCP connection with input/output buffers, `shared_ptr` lifecycle |
| `TcpServer` | Multi-reactor server with accept loop and connection management |
| `Acceptor` | Listening socket handler, runs in base reactor |
| `Buffer` | Non-contiguous read/write buffer with prependable space |
| `Socket` | RAII wrapper around socket fd |
| `InetAddress` | `sockaddr_in` wrapper |
| `HttpRequest` | Simple HTTP 1.0 request parser (GET only) |
| `HttpResponse` | HTTP/1.1 response builder |

## Build & Run

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

All examples are run from the `build/` directory.

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

```cpp
#include "TcpServer.h"
#include "EventLoop.h"
#include "InetAddress.h"

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
├── include/           # Header files
│   ├── noncopyable.h
│   ├── InetAddress.h
│   ├── Socket.h
│   ├── Channel.h
│   ├── Poller.h
│   ├── EPollPoller.h
│   ├── EventLoop.h
│   ├── Timer.h
│   ├── TimerQueue.h
│   ├── Buffer.h
│   ├── TcpConnection.h
│   ├── Acceptor.h
│   ├── TcpServer.h
│   ├── EventLoopThread.h
│   ├── HttpRequest.h
│   └── HttpResponse.h
├── src/               # Library sources (built into libreactornet.a)
│   └── *.cpp
├── example/           # Standalone demo binaries
│   ├── echo_server.cpp
│   └── http_server.cpp
├── tests/             # GoogleTest suites (dev-only, see -DBUILD_TESTING)
│   ├── CMakeLists.txt
│   └── smoke_test.cpp
└── www/               # Static files (HTTP server)
    └── index.html
```

## License

MIT — see [LICENSE](LICENSE).
