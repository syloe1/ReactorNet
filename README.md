# ReactorNet

[![CI](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/syloe1/ReactorNet/actions/workflows/ci.yml)

A minimalist C++ network library implementing the Reactor pattern, inspired by [muduo](https://github.com/chenshuo/muduo), plus a small RPC layer with pluggable payload serializers. No third-party dependencies — pure Linux system calls and the C++17 standard library.

## Features

- **Epoll Event-Driven** — High-performance I/O multiplexing via `epoll`
- **Timer Heap** — Min-heap timer management using `timerfd_create` for kernel-level precision
- **Read/Write Buffers** — Efficient `readv`-based input with automatic growth, handles TCP framing
- **Single Reactor / Multi-Reactor** — One-loop-per-thread architecture with round-robin load distribution
- **HTTP/1.1 Keep-Alive Server** — Static file serving with URL decoding and path traversal protection
- **RPC over TCP** — 16-byte frame header, request-id correlation, sync/async calls, per-call deadlines, and a read-only-after-start service registry (lockless lookups)
- **Pluggable Payload Serializers** — raw opaque bytes (id `0`) and a zero-dependency JSON serializer (id `1`) with a shared `JsonValue` object model
- **Zero Dependencies** — C++17, Linux system calls only; the JSON writer/parser are hand-written

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

### RPC layer

The RPC stack consumes a `TransportPtr`; it never touches a socket directly.
Each concern has exactly one owner:

```
RpcServer / RpcClient            request routing, request-id correlation,
        │                        deadlines, status mapping
        ├── FrameReader         length-prefixed framing (one per connection)
        ├── Codec               frame + envelope: service/method + payload bytes
        └── Serializer          payload <-> JsonValue
                ├── RawSerializer   id 0: opaque bytes (v0.1 default)
                └── JsonSerializer  id 1: JSON text (reference implementation)
                        ├── JsonWriter  JsonValue -> text
                        └── JsonParser  text -> JsonValue
```

The payload format id travels in **byte 6 of the frame header**. A request
declares the format it used; a response must echo the same id. If the two ends
disagree, the client fails the connection instead of handing a mis-decoded
payload to the application. An unknown id in a request gets
`UNSUPPORTED_SERIALIZER` (`0x0D`) and the connection stays usable. See
[RFC-0001](docs/rfc/0001-rpc-protocol.md) for the wire format and
[ADR-0002](docs/adr/0002-serialization.md) for the serializer decision.

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
| `RpcServer` | Server-side RPC chain: `TcpServer` + `FrameReader` + `Codec` + `Serializer` + `ServiceRegistry` ([RFC-0001](docs/rfc/0001-rpc-protocol.md)) |
| `RpcClient` | Client with request-id correlation (many in-flight calls), sync/async APIs and per-call deadlines: `TcpClient` + `FrameReader` + `Codec` + `Serializer` |
| `FrameReader` | Length-prefixed frame assembly with configurable size limits |
| `Codec` | Frame envelope only: `service` / `method` + opaque payload; never parses payload bytes |
| `Serializer` | Payload interface: `JsonValue` <-> bytes, with `id()` on the wire and `name()` for logs |
| `RawSerializer` / `JsonSerializer` | Serializer ids `0` (opaque bytes) and `1` (JSON text) |
| `JsonValue` | Tagged payload object model: null/bool/int64/double/string/array/object, insertion-ordered |
| `JsonWriter` / `JsonParser` | Strict JSON text <-> `JsonValue`: UTF-8 validation, escaping, depth limit 64 |
| `ServiceRegistry` | Name -> handler table, frozen after `start()` so reads need no lock |
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

### RPC Echo

```bash
# Terminal 1: echo service on port 9000 (port 0 picks a free one)
./rpc_echo_server 9000

# Terminal 2: one blocking call on the "Echo.Echo" method
./rpc_echo_client 9000 "hello rpc"
# status=0 payload={"message":"hello rpc","client":"rpc_echo_client"}
# message="hello rpc"
```

The example client configures `JsonSerializer` (id `1`) and the server sets the
same id, so the payload is readable JSON. Both ends default to `RawSerializer`
(id `0`), which carries opaque bytes and keeps the stack usable without JSON.

Responses are correlated by request id, so any number of calls may be in
flight. `Call(request, timeoutSeconds)` and the async variants arm a per-call
deadline; a deadline expiring returns `kTimeout` and leaves the connection open.
A timeout of `0` means no deadline.

Calls can also be issued without blocking:

```cpp
// Callback style: the callback runs on the client's loop thread.
client.AsyncCall(rpc::Request{"Echo", "Echo", "hi"},
                 [](rpc::Response response) { /* ... */ });

// Future style: send now, wait on another thread.
std::future<rpc::Response> future =
    client.CallAsync(rpc::Request{"Echo", "Echo", "hi"});
rpc::Response response = future.get();
```

Callbacks must not block or call the synchronous `Call()`: the response they
would wait for is delivered by that same loop thread. Issuing another
`AsyncCall()` from a callback is fine. C++17 futures have no continuation, so
`CallAsync()` only offers `get()`/`wait()`.

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
├── rpc/
│   ├── include/rpc/         # RPC public headers
│   │   ├── protocol.h  frame_reader.h  codec.h
│   │   ├── serializer.h
│   │   ├── json_value.h  json_writer.h  json_parser.h
│   │   ├── json_serializer.h  service_registry.h
│   │   └── rpc_server.h  rpc_client.h
│   └── src/                 # RPC sources -> libreactornet_rpc.a
├── example/
│   ├── echo_server.cpp  http_server.cpp
│   └── rpc_echo_server.cpp  rpc_echo_client.cpp
├── tests/                   # Core GoogleTest suites (dev-only)
│   └── rpc/                 # Protocol, serializer, JSON, server/client suites
├── bench/
│   └── serialization_bench.cpp
├── docs/
│   ├── rfc/                 # Protocol, layering, transport contracts
│   └── adr/                 # Registration and serialization decisions
└── www/                     # Static files (HTTP server)
```

## Documentation

| Document | What it answers |
|----------|-----------------|
| [Learn.md](Learn.md) | Guided reading order: which file to open, why it exists, what to try |
| [RFC-0001](docs/rfc/0001-rpc-protocol.md) | Exact wire format, status codes, reject/tolerate rules |
| [RFC-0002](docs/rfc/0002-rpc-layering.md) | Which code belongs to which layer, thread ownership, error classes |
| [RFC-0003](docs/rfc/0003-transport-abstraction.md) | The `Transport` interface and its thread contract |
| [ADR-0001](docs/adr/0001-service-registration.md) | How a typed method call becomes a byte-level registry entry |
| [ADR-0002](docs/adr/0002-serialization.md) | Why the reference implementation hand-writes JSON, and the plugin plan |

## License

MIT — see [LICENSE](LICENSE).
