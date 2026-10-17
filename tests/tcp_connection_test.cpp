#include <gtest/gtest.h>

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpConnection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace reactornet;

// Tests for the TcpConnection send path (issue #23).
//
// A TcpConnection only exists as a shared_ptr owned by its loop thread (that is
// how TcpServer stores it), and every claim under test here is about that
// ownership or about ordering across threads. So this fixture does not fake a
// connection object: it builds a real loopback TCP pair, hands the accepted end
// to a TcpConnection living on an EventLoopThread, and keeps the client end for
// the test thread to read from.
//
// What is intentionally NOT used: fixed ports (they collide between concurrent
// ctest runs) and bare sleeps (every wait is an event with a deadline).
namespace {

using namespace std::chrono_literals;

// Real latencies here are microseconds; a wait still unsatisfied after 3s is a
// hang and must fail the case rather than block the whole test binary.
constexpr auto kWaitTimeout = 3s;
// Generous enough for 4MB through loopback, small enough that a broken write
// path fails instead of hanging.
constexpr auto kReadTimeout = 10s;

class TcpConnectionSendTest : public ::testing::Test {
protected:
  void SetUp() override {
    // --- A real TCP pair on loopback -------------------------------------
    // The listener stays blocking: connect() below completes against the
    // backlog, so the test thread never needs to accept concurrently.
    int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0) << "socket(): " << std::strerror(errno);
    listenerFd_ = listener;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // let the kernel pick, so parallel runs cannot collide
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
              0)
        << "bind(): " << std::strerror(errno);
    ASSERT_EQ(::listen(listener, 1), 0) << "listen(): " << std::strerror(errno);
    socklen_t addrLen = sizeof(addr);
    ASSERT_EQ(
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &addrLen),
        0)
        << "getsockname(): " << std::strerror(errno);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0) << "socket(): " << std::strerror(errno);
    clientFd_ = client;
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
              0)
        << "connect(): " << std::strerror(errno);

    int server =
        ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    ASSERT_GE(server, 0) << "accept4(): " << std::strerror(errno);
    serverFd_ = server;
    ::close(listener);
    listenerFd_ = -1;

    // A small send buffer makes the first write of the large-message case
    // partial deterministically, instead of relying on the kernel's autotuned
    // buffer size being smaller than the payload on this machine. The kernel
    // doubles the requested value, so the first write accepts ~64KB.
    int sndbuf = 32 * 1024;
    ASSERT_EQ(::setsockopt(server, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)),
              0)
        << "setsockopt(SO_SNDBUF): " << std::strerror(errno);

    int flags = ::fcntl(clientFd_, F_GETFL, 0);
    ASSERT_GE(flags, 0) << "fcntl(F_GETFL): " << std::strerror(errno);
    ASSERT_EQ(::fcntl(clientFd_, F_SETFL, flags | O_NONBLOCK), 0)
        << "fcntl(F_SETFL): " << std::strerror(errno);

    sockaddr_in localAddr{};
    sockaddr_in peerAddr{};
    socklen_t localLen = sizeof(localAddr);
    socklen_t peerLen = sizeof(peerAddr);
    ASSERT_EQ(::getsockname(server, reinterpret_cast<sockaddr *>(&localAddr),
                            &localLen),
              0);
    ASSERT_EQ(::getpeername(server, reinterpret_cast<sockaddr *>(&peerAddr),
                            &peerLen),
              0);

    // --- The connection, on its loop thread ------------------------------
    loop_ = loopThread_.startLoop();

    auto ready = std::make_shared<std::promise<void>>();
    std::future<void> readyFuture = ready->get_future();
    loop_->runInLoop([this, server, local = InetAddress(localAddr),
                      peer = InetAddress(peerAddr), ready] {
      // Created, connected and owned on the loop thread, mirroring how
      // TcpServer keeps its connections. owner_ is the strong reference; conn_
      // only publishes the raw pointer after construction, for the test thread
      // to call thread-safe methods on.
      owner_ = std::make_shared<TcpConnection>(loop_, "TcpConnectionSendTest",
                                               server, local, peer);
      conn_ = owner_.get();
      owner_->connectEstablished();
      ready->set_value();
    });
    ASSERT_EQ(readyFuture.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop thread never established the connection";
    // The TcpConnection's Socket owns the fd from here on.
    serverFd_ = -1;
  }

  void TearDown() override {
    // Tear the connection down on its loop thread, in TcpServer's order:
    // connectDestroyed() removes it from the poller, then the owner's last
    // strong reference goes away. A case that already did this leaves owner_
    // and conn_ null, so this is a no-op there.
    if (loop_ != nullptr) {
      auto done = std::make_shared<std::promise<void>>();
      std::future<void> teardown = done->get_future();
      loop_->runInLoop([this, done] {
        if (owner_) {
          owner_->connectDestroyed();
          owner_.reset();
        }
        conn_ = nullptr;
        done->set_value();
      });
      if (teardown.wait_for(kWaitTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "the loop did not tear down the connection in time";
      }
    }
    if (clientFd_ >= 0) {
      ::close(clientFd_);
      clientFd_ = -1;
    }
    if (serverFd_ >= 0) {
      // Only reached when setup failed before the TcpConnection took over.
      ::close(serverFd_);
      serverFd_ = -1;
    }
    if (listenerFd_ >= 0) {
      ::close(listenerFd_);
      listenerFd_ = -1;
    }
    // ~EventLoopThread now quits the loop and joins the thread; the connection
    // is already gone, so the loop has nothing left that asserts on teardown.
  }

  // Queue a send on the loop thread and wait for sendInLoop to return. Used to
  // exercise the true in-loop path, with no cross-thread hop.
  void sendOnLoop(const std::string &message) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> returned = done->get_future();
    loop_->runInLoop([this, message, done] {
      conn_->send(message);
      done->set_value();
    });
    ASSERT_EQ(returned.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not process the send in time";
  }

  // Read until `expected` bytes arrive or the deadline passes. Returns what was
  // actually received; the caller checks the size before comparing content so a
  // failure does not print a multi-megabyte diff.
  std::string readExact(size_t expected,
                        std::chrono::milliseconds timeout = kReadTimeout) {
    std::string received;
    received.reserve(expected);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[16 * 1024];
    while (received.size() < expected) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto remainingMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      pollfd pfd{clientFd_, POLLIN, 0};
      const int pollResult = ::poll(&pfd, 1, static_cast<int>(remainingMs));
      if (pollResult < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (pollResult == 0) {
        break;
      }
      const ssize_t n = ::recv(clientFd_, buffer, sizeof(buffer), 0);
      if (n > 0) {
        received.append(buffer, static_cast<size_t>(n));
      } else if (n == 0) {
        break; // peer closed
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
    }
    return received;
  }

  EventLoopThread loopThread_;
  EventLoop *loop_ = nullptr;
  TcpConnection::TcpConnectionPtr owner_; // loop-thread only
  TcpConnection *conn_ = nullptr;         // published for the test thread
  int listenerFd_ = -1;
  int serverFd_ = -1;
  int clientFd_ = -1;
};

// --- Fast path --------------------------------------------------------------

TEST_F(TcpConnectionSendTest, SmallMessageFromTheLoopThreadIsDelivered) {
  const std::string payload = "hello inline send";
  sendOnLoop(payload);

  const std::string received = readExact(payload.size());
  ASSERT_EQ(received.size(), payload.size())
      << "the peer never received the whole message";
  EXPECT_TRUE(received == payload) << "the peer received different bytes";
}

TEST_F(TcpConnectionSendTest, CrossThreadSendIsQueuedAndDelivered) {
  const std::string payload = "sent from the test thread";

  // No runInLoop here: send() itself must take the cross-thread branch, queue
  // the work and still deliver the message.
  conn_->send(payload);

  const std::string received = readExact(payload.size());
  ASSERT_EQ(received.size(), payload.size())
      << "the peer never received the whole message";
  EXPECT_TRUE(received == payload) << "the peer received different bytes";
}

// --- Partial write ----------------------------------------------------------

TEST_F(TcpConnectionSendTest, LargeMessageIsBufferedAndFullyDeliveredAfterPartialWrite) {
  // SO_SNDBUF was lowered in SetUp, so writing this in one call must accept
  // only part of it. sendInLoop has to buffer the remainder, arm EPOLLOUT, and
  // finish the job as the peer drains — and it must return promptly rather
  // than block the loop until the whole payload fits.
  const std::string payload(1024 * 1024, 'x');
  sendOnLoop(payload);

  const std::string received = readExact(payload.size());
  ASSERT_EQ(received.size(), payload.size())
      << "the buffered remainder was never written out";
  EXPECT_TRUE(received == payload) << "the peer received different bytes";
}

// --- Lifetime ---------------------------------------------------------------

TEST_F(TcpConnectionSendTest, CrossThreadSendKeepsTheConnectionAliveWhenTheOwnerGoesAway) {
  // Regression test for the raw-`this` capture in send(). The loop is blocked
  // inside a gate so both tasks below are queued before either can run; their
  // order is then fixed by the queue: release the owner first, run the send
  // second. When the send lambda runs, the connection has already been
  // disconnected and its last owner is gone.
  auto gateEntered = std::make_shared<std::promise<void>>();
  auto gateRelease = std::make_shared<std::promise<void>>();
  std::future<void> entered = gateEntered->get_future();

  loop_->runInLoop([gateEntered, gateRelease] {
    gateEntered->set_value();
    gateRelease->get_future().wait();
  });
  ASSERT_EQ(entered.wait_for(kWaitTimeout), std::future_status::ready)
      << "the loop never reached the gate";

  loop_->runInLoop([this] {
    if (owner_) {
      owner_->connectDestroyed(); // disconnect and remove from the poller
      owner_.reset();             // drop the fixture's strong reference
    }
    conn_ = nullptr; // TearDown must not touch a destroyed object
  });

  // The loop is still blocked, so the connection is still kConnected here and
  // this call takes the cross-thread path.
  conn_->send("queued while the owner is about to go away");

  gateRelease->set_value();

  // A sentinel proves the loop survived the queued send. If the send lambda
  // touched a freed connection, ASan aborts before the sentinel can run (and
  // TSan flags the unsynchronised state read this capture would also be part
  // of).
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> returned = done->get_future();
  loop_->runInLoop([done] { done->set_value(); });
  EXPECT_EQ(returned.wait_for(kWaitTimeout), std::future_status::ready)
      << "the loop did not survive the queued send";
}

// --- Cross-thread state -----------------------------------------------------

TEST_F(TcpConnectionSendTest, ConcurrentSendsAndForceCloseDoNotRace) {
  // Four senders and a closer hammer state_ from different threads while the
  // loop thread reads and writes it too. The point is not byte accounting —
  // forceClose is allowed to drop queued output — but that no unsynchronised
  // access exists. Under TSan, a plain StateE here is a reported data race.
  constexpr int kSenders = 4;
  constexpr int kMessagesPerSender = 200;

  std::vector<std::thread> senders;
  senders.reserve(kSenders);
  for (int i = 0; i < kSenders; ++i) {
    senders.emplace_back([this] {
      for (int message = 0; message < kMessagesPerSender; ++message) {
        conn_->send("ping");
      }
    });
  }

  std::thread closer([this] {
    for (int i = 0; i < 20; ++i) {
      conn_->forceClose();
      std::this_thread::yield();
    }
  });

  for (auto &sender : senders) {
    sender.join();
  }
  closer.join();

  // Reaching here without a crash or sanitizer report is the assertion. Let the
  // loop drain whatever ordering it ended up with before TearDown runs.
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> returned = done->get_future();
  loop_->runInLoop([done] { done->set_value(); });
  ASSERT_EQ(returned.wait_for(kWaitTimeout), std::future_status::ready);
}

} // namespace
