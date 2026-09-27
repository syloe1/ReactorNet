#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpConnection.h"
#include "TcpServer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// End-to-end tests over a real TCP socket.
//
// The other suites in this directory test parts: Buffer moves bytes, TimerQueue
// orders a heap, EventLoop posts a functor. None of them run the chain that the
// library actually is --
//
//   accept -> round-robin to a worker -> readFd -> message callback -> send ->
//   handleWrite -> close
//
// -- and that chain is where the interesting failures live, because they need a
// real socket and a real kernel to show up: EAGAIN on a full send buffer,
// EPOLLOUT being armed and disarmed at the right moments, and TcpConnection
// staying alive across a callback that drops the last reference to it. So there
// is no mock here: each case starts a real TcpServer on a real port and talks to
// it with a plain POSIX client socket.
//
// Two consequences shape everything below.
//
// Ports are never hardcoded. The server is built with port 0 and the test reads
// back the port the kernel picked (TcpServer::listenPort()), so two ctest runs
// on one machine, or a busy CI runner, cannot collide.
//
// Nothing waits on a bare sleep. Every server-side observation is a
// condition_variable with a deadline, and every client-side read is a poll()
// against the same deadline, so a wedged loop shows up as a failed assertion
// after kDeadline rather than as a job that hangs until the runner kills it.
// (tests/CMakeLists.txt caps each case with a ctest TIMEOUT as the backstop.)
//
// Known gap, recorded here rather than papered over: the multi-client case
// asserts that every client gets its own bytes back, which is what catches
// cross-talk between connections. It does *not* assert that the four workers
// were all used -- pinning TcpServer::newConnection's round-robin to
// subLoops_[0] would still pass it. Covering dispatch balance properly means
// counting accepts per worker, which is a benchmark-time measurement, not
// something worth a brittle assertion here.
namespace {

using namespace std::chrono_literals;

using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

// Every round trip in this file gets this long. Real latencies on loopback are
// microseconds, so this is pure headroom: it is short enough that a broken case
// fails the suite quickly, and long enough that a loaded CI runner will not
// trip it.
constexpr auto kDeadline = 5s;

// Backstop for fixture setup only. Deliberately above kPollTimeMs (10s in
// EventLoop.cpp): if a setup task's wakeup were ever lost, the loop would still
// get there one poll timeout later, and this wait must not be the thing that
// reports it.
constexpr auto kStartupTimeout = 12s;

// A count that can be waited on. Callbacks arrive on whichever worker loop owns
// the connection -- not on the test thread -- so every counter in this file has
// to carry its own lock and condition variable, and the test thread waits on
// that instead of on a sleep.
class Counter {
public:
  void bump() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    cond_.notify_all();
  }

  // Returns false on timeout. Callers assert on that: "did not arrive in time"
  // is a failure this suite must report, never something to block on.
  bool waitForAtLeast(int target, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cond_.wait_for(lock, timeout, [&] { return count_ >= target; });
  }

  int value() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  int count_ = 0;
};

// Tracks how many connections the server currently believes are open, so
// teardown can wait for the server to have finished with them. See stopServer().
class LiveConnections {
public:
  void opened() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++live_;
    cond_.notify_all();
  }

  void closed() {
    std::lock_guard<std::mutex> lock(mutex_);
    --live_;
    cond_.notify_all();
  }

  bool waitUntilZero(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cond_.wait_for(lock, timeout, [&] { return live_ <= 0; });
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  int live_ = 0;
};

// A plain non-blocking client socket, written straight against the syscall API.
// Deliberately shares no code with ReactorNet: the point of this file is to
// exercise the library across a real TCP stack, and a client built out of the
// library under test could pass while both sides were wrong in the same way.
//
// Every operation is bounded by kDeadline. Errors are returned as strings
// rather than asserted on, because the caller is a gtest body that needs to
// report them with context (and worker threads must not assert at all).
class ClientSocket {
public:
  ClientSocket() = default;
  ~ClientSocket() { close(); }
  ClientSocket(const ClientSocket &) = delete;
  ClientSocket &operator=(const ClientSocket &) = delete;

  bool connectTo(uint16_t port, std::string *error) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   IPPROTO_TCP);
    if (fd_ < 0) {
      *error = std::string("socket: ") + std::strerror(errno);
      return false;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      if (errno != EINPROGRESS) {
        *error = std::string("connect: ") + std::strerror(errno);
        return false;
      }
      if (!waitFor(POLLOUT, std::chrono::steady_clock::now() + kDeadline,
                   error)) {
        return false;
      }
      // A non-blocking connect reports its outcome on the socket, not in
      // errno from connect(). Without this check a refused connection would
      // look like a success and fail later as a confusing timeout.
      int soError = 0;
      socklen_t len = sizeof(soError);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soError, &len) < 0) {
        *error = std::string("getsockopt(SO_ERROR): ") + std::strerror(errno);
        return false;
      }
      if (soError != 0) {
        *error = std::string("connect: ") + std::strerror(soError);
        return false;
      }
    }

    // Nagle would let the kernel hold a small write back looking for company,
    // which is exactly the behaviour the split-packet case is trying to rule
    // out as a source of noise.
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
  }

  // Sends everything, retrying on EAGAIN with a bounded wait for writability.
  // The retry is the client half of the problem the server solves with
  // EPOLLOUT: a large enough payload cannot fit in one socket buffer.
  bool sendAll(const std::string &data, std::string *error) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n =
          ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (!waitFor(POLLOUT, deadline, error)) {
            return false;
          }
          continue;
        }
        *error = std::string("send: ") + std::strerror(errno);
        return false;
      }
      *error = "send returned 0 for a non-empty buffer";
      return false;
    }
    return true;
  }

  // Reads exactly `len` bytes into *out. On failure *out keeps whatever arrived
  // first, so a caller can report how much of the payload actually made it.
  bool recvExactly(size_t len, std::string *out, std::string *error) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    out->clear();
    char buf[65536];
    while (out->size() < len) {
      const size_t want = std::min(sizeof(buf), len - out->size());
      const ssize_t n = ::recv(fd_, buf, want, 0);
      if (n > 0) {
        out->append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        *error = "server closed after " + std::to_string(out->size()) + " of " +
                 std::to_string(len) + " bytes";
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!waitFor(POLLIN, deadline, error)) {
          return false;
        }
        continue;
      }
      *error = std::string("recv: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  bool waitFor(short events, std::chrono::steady_clock::time_point deadline,
               std::string *error) {
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        *error = "timed out waiting for the server";
        return false;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - now);
      pollfd pfd{fd_, events, 0};
      const int n = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        *error = std::string("poll: ") + std::strerror(errno);
        return false;
      }
      if (n == 0) {
        *error = "timed out waiting for the server";
        return false;
      }
      if (pfd.revents & (POLLERR | POLLNVAL)) {
        *error = "socket error while waiting for the server";
        return false;
      }
      if (pfd.revents & events) {
        return true;
      }
      if (pfd.revents & POLLHUP) {
        *error = "server hung up";
        return false;
      }
      // Neither what we asked for nor anything fatal -- loop round and let the
      // deadline decide.
    }
  }

  int fd_ = -1;
};

// Position-dependent payload, so a truncated, duplicated or reordered stream
// cannot coincide with the expected bytes. Kept printable so a mismatch is
// readable in the failure output.
std::string makePayload(size_t len, uint32_t seed) {
  std::string payload(len, '\0');
  uint32_t x = seed;
  for (size_t i = 0; i < len; ++i) {
    x = x * 1664525u + 1013904223u;
    payload[i] = static_cast<char>('!' + (x >> 24) % 90);
  }
  return payload;
}

// Echoes whatever arrives, the standard identity callback.
void echoBack(const TcpConnectionPtr &conn, Buffer *buf, Timestamp) {
  conn->send(buf->retrieveAllAsString());
}

class IntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    baseThread_ = std::make_unique<EventLoopThread>();
    baseLoop_ = baseThread_->startLoop();
  }

  // Builds and starts a server, and returns the port the kernel assigned.
  //
  // Everything that touches TcpServer runs on the base loop thread: the
  // constructor binds the listening socket, start() arms it, and ~TcpServer
  // asserts it is being called there. Posting it as one task also removes a
  // race -- start() arms the acceptor through runInLoop, so returning from it on
  // the base loop thread is the only point at which a client is guaranteed to
  // be able to connect rather than get ECONNREFUSED.
  uint16_t startServer(int threadNum, TcpConnection::MessageCallback onMessage,
                       TcpConnection::ConnectionCallback onConnection = nullptr) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> ready = done->get_future();

    baseLoop_->runInLoop([this, threadNum, onMessage = std::move(onMessage),
                          onConnection = std::move(onConnection), done] {
      server_ = std::make_unique<TcpServer>(
          baseLoop_, InetAddress(0, "127.0.0.1"), "IntegrationTest");

      // Wrapped rather than replaced: teardown has to know how many connections
      // the server still holds, and only this callback can tell it.
      server_->setConnectionCallback(
          [this, onConnection](const TcpConnectionPtr &conn) {
            if (conn->connected()) {
              live_.opened();
            } else {
              live_.closed();
            }
            if (onConnection) {
              onConnection(conn);
            }
          });
      server_->setMessageCallback(std::move(onMessage));
      server_->setThreadNum(threadNum);
      server_->start();
      port_ = server_->listenPort();
      done->set_value();
    });

    EXPECT_EQ(ready.wait_for(kStartupTimeout), std::future_status::ready)
        << "the base loop never ran the server setup task";
    return port_;
  }

  void TearDown() override {
    if (server_) {
      stopServer();
    }
    baseThread_.reset(); // quits the base loop and joins its thread
  }

  // Shuts the server down in an order that keeps TcpServer's own bookkeeping
  // consistent. The subtlety is that TcpServer::removeConnection posts
  // removeConnectionInLoop to the base loop with a *raw* TcpServer::this
  // captured. Dropping the server while one of those is still queued runs a
  // method on a destroyed map, so teardown waits for every queued removal to
  // have landed before the destructor runs, rather than hoping it won.
  void stopServer() {
    // 1. Wait until the server has seen every accepted client leave. This is
    //    what covers the gap where a client's FIN is still in flight: without
    //    it, the worker might not have reached handleClose() yet, and that is
    //    where the removal request is posted from.
    live_.waitUntilZero(kDeadline);

    // 2. Drain the worker loops first. handleClose() runs inside an event
    //    handler, and a functor queued behind it cannot run until that handler
    //    returns, so once these return, every removal the workers owed the base
    //    loop has been posted.
    std::vector<EventLoop *> loops;
    runOnBaseLoopAndWait([this, &loops] { loops = server_->getAllLoops(); });
    for (EventLoop *loop : loops) {
      if (loop != baseLoop_) {
        drainLoop(loop);
      }
    }

    // 3. Then drain the base loop, which runs those removals in the order they
    //    were posted.
    drainLoop(baseLoop_);

    // 4. Only now is it safe to destroy the server, on the thread whose
    //    affinity ~TcpServer checks.
    runOnBaseLoopAndWait([this] { server_.reset(); });
  }

  // Runs fn on the base loop thread and blocks until it has run.
  template <typename Fn> void runOnBaseLoopAndWait(Fn &&fn) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    baseLoop_->runInLoop([&fn, done] {
      fn();
      done->set_value();
    });
    finished.wait_for(kStartupTimeout);
  }

  // Blocks until the loop has worked through everything queued ahead of this.
  void drainLoop(EventLoop *loop) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    loop->runInLoop([done] { done->set_value(); });
    finished.wait_for(kDeadline);
  }

  std::unique_ptr<EventLoopThread> baseThread_;
  EventLoop *baseLoop_ = nullptr;
  std::unique_ptr<TcpServer> server_;
  uint16_t port_ = 0;
  LiveConnections live_;
};

// The baseline: bytes in, the same bytes out.
TEST_F(IntegrationTest, EchoRoundTrip) {
  const uint16_t port = startServer(0, echoBack);
  ASSERT_NE(port, 0) << "the server never reported a bound port";

  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(port, &error)) << error;

  const std::string payload = "Hello, Reactor!";
  ASSERT_TRUE(client.sendAll(payload, &error)) << error;

  std::string received;
  ASSERT_TRUE(client.recvExactly(payload.size(), &received, &error)) << error;
  EXPECT_EQ(received, payload);
}

// One logical message, handed to the kernel as two writes. What is being
// checked is that the server reassembles the stream rather than losing or
// duplicating the first fragment.
TEST_F(IntegrationTest, SplitWriteIsReassembled) {
  const std::string first = "the quick brown ";
  const std::string second = "fox jumps over the lazy dog";
  const std::string expected = first + second;

  auto messages = std::make_shared<Counter>();
  // Legitimately shared mutable state only because this case has exactly one
  // client: every callback below is the same connection, on one worker loop.
  // Held by shared_ptr, and the expected length captured by value, because this
  // callback lives inside the server -- which outlives the test body, and so
  // outlives every local in it.
  auto assembled = std::make_shared<std::string>();
  const size_t expectedSize = expected.size();

  // The server answers only once it holds the whole message, which is what
  // makes this about reassembly: a server that dropped the first fragment would
  // never reply at all, and the client would time out rather than quietly
  // getting half its data back.
  const uint16_t port = startServer(
      0, [assembled, messages, expectedSize](const TcpConnectionPtr &conn,
                                             Buffer *buf, Timestamp) {
        messages->bump();
        assembled->append(buf->retrieveAllAsString());
        if (assembled->size() >= expectedSize) {
          conn->send(*assembled);
        }
      });
  ASSERT_NE(port, 0);

  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(port, &error)) << error;

  ASSERT_TRUE(client.sendAll(first, &error)) << error;

  // Waiting for the server to have seen the first fragment, rather than
  // sleeping to "let it arrive", is what makes the split deterministic: the
  // second write cannot be coalesced into the first read if it is not sent
  // until that read has already been handled.
  ASSERT_TRUE(messages->waitForAtLeast(1, kDeadline))
      << "the server never reported the first fragment";
  ASSERT_TRUE(client.sendAll(second, &error)) << error;

  std::string received;
  ASSERT_TRUE(client.recvExactly(expected.size(), &received, &error)) << error;
  EXPECT_EQ(received, expected);

  // At least two: the sequencing above guarantees the first fragment was
  // consumed before the second was sent, so they cannot have arrived as one
  // read. Recorded rather than asserted exactly, because TCP is still free to
  // split either write further.
  EXPECT_GE(messages->value(), 2);
  RecordProperty("server_message_callbacks", messages->value());
}

// 256KB in one direction. This is the case that reaches the parts of the
// library the small payloads never touch: readFd's 64KB extrabuf branch (the
// input buffer starts at 1KB, so any decent read overflows it immediately) and
// makeSpace() growing that buffer afterwards.
//
// Note what it does *not* reach, because it is easy to assume otherwise: the
// server's writes here all complete in a single write(). A short write needs the
// buffered output to exceed the socket's send buffer, and the kernel autotunes
// SO_SNDBUF to a ceiling of net.ipv4.tcp_wmem[2] -- 4MB by default, measured at
// 2.5MB on the machine this was written on. 256KB fits no matter what, and
// shrinking the *client's* SO_RCVBUF does not change it (measured: the server's
// send buffer stayed at 2.5MB with a receive buffer anywhere from 1 byte to
// 8KB). Partial writes are covered by LargeBacklogIsResumedAcrossEventLoopTurns
// below, which is sized to actually get there.
TEST_F(IntegrationTest, LargeTransferIsEchoedByteForByte) {
  const uint16_t port = startServer(0, echoBack);
  ASSERT_NE(port, 0);

  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(port, &error)) << error;

  const std::string payload = makePayload(256 * 1024, 0x5eedu);
  ASSERT_TRUE(client.sendAll(payload, &error)) << error;

  std::string received;
  ASSERT_TRUE(client.recvExactly(payload.size(), &received, &error)) << error;

  // Compared by offset rather than as two blobs: a failure here should say
  // *where* the stream diverged, and dumping 256KB of payload into the test log
  // helps nobody.
  size_t difference = 0;
  while (difference < payload.size() && difference < received.size() &&
         received[difference] == payload[difference]) {
    ++difference;
  }
  EXPECT_EQ(received.size(), payload.size())
      << "first divergence at byte " << difference;
  EXPECT_EQ(difference, payload.size())
      << "echoed bytes diverged at offset " << difference << " of "
      << payload.size();
}

// The partial-write path: handleWrite() putting part of outputBuffer_ on the
// wire, keeping the rest, and resuming when EPOLLOUT says the socket has room
// again. Two things have to line up for it.
//
// The buffered output has to exceed the socket's send buffer. The kernel
// autotunes SO_SNDBUF up to net.ipv4.tcp_wmem[2] -- 4MB by default -- so the
// payload is 8MB, twice that ceiling, which is what makes this land on any
// host running default sysctls rather than only on the one it was written on.
//
// And the client has to not read while the server is writing, or the server
// drains as it goes and the output buffer never builds up. Hence sending
// everything before reading anything.
//
// Measured on this machine with sndbuf at 2.5MB: seven short writes, the worst
// leaving 2.7MB behind (offered=4313187 wrote=1564457). With the payload cut to
// 256KB the same code produces zero short writes, which is why the case above
// does not claim to cover this.
//
// The assertion is the same as the small cases -- every byte comes back in
// order -- but it is testing something different: if the remainder were dropped
// instead of retained, or EPOLLOUT were not re-armed, the transfer would stall
// and this would time out rather than fail on content.
TEST_F(IntegrationTest, LargeBacklogIsResumedAcrossEventLoopTurns) {
  const uint16_t port = startServer(0, echoBack);
  ASSERT_NE(port, 0);

  ClientSocket client;
  std::string error;
  ASSERT_TRUE(client.connectTo(port, &error)) << error;

  const std::string payload = makePayload(8u * 1024 * 1024, 0xfeedu);
  ASSERT_TRUE(client.sendAll(payload, &error)) << error;

  std::string received;
  ASSERT_TRUE(client.recvExactly(payload.size(), &received, &error)) << error;

  size_t difference = 0;
  while (difference < payload.size() && difference < received.size() &&
         received[difference] == payload[difference]) {
    ++difference;
  }
  EXPECT_EQ(received.size(), payload.size())
      << "first divergence at byte " << difference
      << " -- a short transfer here means the unsent remainder was dropped "
         "rather than kept in outputBuffer_ for the next EPOLLOUT";
  EXPECT_EQ(difference, payload.size())
      << "echoed bytes diverged at offset " << difference << " of "
      << payload.size();
}

// Eight clients at once across four worker loops, each with its own payload.
// This is the cross-talk check: bytes belonging to one connection must never
// surface on another, which is what a mistake in per-connection buffer or
// channel ownership would look like.
TEST_F(IntegrationTest, EightConcurrentClientsEachGetTheirOwnEcho) {
  const uint16_t port = startServer(4, echoBack);
  ASSERT_NE(port, 0);

  constexpr int kClients = 8;
  std::vector<std::string> payloads;
  payloads.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    payloads.push_back(makePayload(4096, 1000u + static_cast<uint32_t>(i)));
  }

  // One slot per client, written only by that client's thread, so the workers
  // need no lock between them. gtest assertions cannot be used off the test
  // thread, so failures are recorded here and asserted after the joins.
  std::vector<std::string> failures(kClients);
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&, i] {
      ClientSocket client;
      std::string error;
      if (!client.connectTo(port, &error)) {
        failures[i] = "connect: " + error;
        return;
      }
      if (!client.sendAll(payloads[i], &error)) {
        failures[i] = "send: " + error;
        return;
      }
      std::string received;
      if (!client.recvExactly(payloads[i].size(), &received, &error)) {
        failures[i] = "recv: " + error;
        return;
      }
      if (received != payloads[i]) {
        failures[i] = "received " + std::to_string(received.size()) +
                      " bytes that did not match this client's payload";
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  for (int i = 0; i < kClients; ++i) {
    EXPECT_TRUE(failures[i].empty()) << "client " << i << ": " << failures[i];
  }
}

// A client hanging up must be reported to the connection callback, exactly
// once, and must leave the server able to serve the next client.
TEST_F(IntegrationTest, ClientCloseIsNoticedAndServerKeepsServing) {
  auto opened = std::make_shared<Counter>();
  auto closed = std::make_shared<Counter>();

  const uint16_t port = startServer(
      0, echoBack, [opened, closed](const TcpConnectionPtr &conn) {
        if (conn->connected()) {
          opened->bump();
        } else {
          closed->bump();
        }
      });
  ASSERT_NE(port, 0);

  {
    ClientSocket client;
    std::string error;
    ASSERT_TRUE(client.connectTo(port, &error)) << error;
    ASSERT_TRUE(opened->waitForAtLeast(1, kDeadline))
        << "the server never reported the connection";
    // Falling out of scope closes the socket, which is the hangup under test.
  }

  ASSERT_TRUE(closed->waitForAtLeast(1, kDeadline))
      << "the server never reported the disconnect";

  // Exactly one. TcpConnection reaches the callback by two routes -- handleClose
  // and connectDestroyed -- and a connection that reported its own death twice
  // would corrupt any user-side bookkeeping built on this callback.
  EXPECT_EQ(closed->value(), 1)
      << "one closed connection must produce one disconnect notification";
  EXPECT_EQ(opened->value(), 1);

  // The process still being alive is not on its own evidence that the server is
  // healthy, so this asserts it properly: a fresh connection has to be accepted
  // and served normally.
  ClientSocket second;
  std::string error;
  ASSERT_TRUE(second.connectTo(port, &error)) << error;
  const std::string payload = "still here";
  ASSERT_TRUE(second.sendAll(payload, &error)) << error;

  std::string received;
  ASSERT_TRUE(second.recvExactly(payload.size(), &received, &error)) << error;
  EXPECT_EQ(received, payload) << "the server stopped serving after a disconnect";
}

} // namespace

