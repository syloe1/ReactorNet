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
```

- **baseLoop（main 线程）**：只做`accept`，不处理业务 IO。
- **worker EventLoop**：每个线程一个 epoll 实例，负责已连接 socket 的读写事件。
**连接一旦分派到某个 worker，该连接所有 IO 事件永远在这个 worker 线程执行**，业务回调都跑在 IO 线程，不用加锁。

### Transport layer

Application code (HTTP server, RPC session) consumes a connection as a
`TransportPtr`, a transport-agnostic byte stream:

```
Application (HTTP / RPC session)
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
| `RpcServer` | Server-side RPC chain: `TcpServer` + `FrameReader` + `Codec` + `ServiceRegistry` ([RFC-0001](docs/rfc/0001-rpc-protocol.md)) |
| `RpcClient` | Blocking client with request-id correlation (many in-flight calls): `TcpClient` + `FrameReader` + `Codec` |
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

### RPC Echo

```bash
# Terminal 1: echo service on port 9000 (port 0 picks a free one)
./rpc_echo_server 9000

# Terminal 2: one blocking call on the "Echo.Echo" method
./rpc_echo_client 9000 "hello rpc"
# status=0 payload="hello rpc"
```

Responses are correlated by request id, so any number of calls may be in flight; v0.1 has no response timeout.

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
│   ├── Transport.h
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
