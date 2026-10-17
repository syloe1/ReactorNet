#include <gtest/gtest.h>

#include "Connector.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <vector>
#include <unistd.h>

using namespace reactornet;

// Tests for the client-side Connector (issue #24).
//
// The Connector lives on an EventLoopThread, like it will under TcpClient.
// Every wait is an event with a deadline; no test waits on the real network's
// SYN timeout.
namespace {

using namespace std::chrono_literals;

// Loopback handshakes are microseconds; anything still pending after 3s is a
// hang and should fail the case rather than block the binary.
constexpr auto kWaitTimeout = 3s;

sockaddr_in loopbackAddress(uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  return addr;
}

// Listening socket on an ephemeral loopback port. Returns -1 on failure and
// leaves errno set.
int makeListener(int backlog, uint16_t *port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr = loopbackAddress(0);
  socklen_t len = sizeof(addr);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, backlog) < 0 ||
      ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}

// A port held by a bound-but-never-listening socket. connect() to it is
// refused by the kernel, and no other process can take the port while this
// socket stays open — so the test does not race with anything.
int makeRefusingSocket(uint16_t *port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr = loopbackAddress(0);
  socklen_t len = sizeof(addr);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
      ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}

int countOpenFds() {
  DIR *dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return -1;
  }
  int count = 0;
  while (dirent *entry = ::readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    ++count;
  }
  ::closedir(dir);
  return count;
}

int readIntFile(const char *path) {
  std::ifstream in(path);
  int value = -1;
  if (!(in >> value)) {
    return -1;
  }
  return value;
}

struct ConnectResult {
  std::promise<void> done;
  int sockfd = -1;
  int error = 0;
};

// A listener whose accept queue is deliberately kept full. Further connection
// attempts get no answer: their SYNs are dropped, so they neither complete nor
// receive an errno from the peer. That makes both the timeout path and an
// asynchronous failure (close the listener while a connect is in flight)
// testable without depending on a real network.
struct BlackHole {
  int listener = -1;
  uint16_t port = 0;
  std::vector<int> fillers;

  ~BlackHole() {
    if (listener >= 0) {
      ::close(listener);
    }
    for (int fd : fillers) {
      ::close(fd);
    }
  }
};

bool buildBlackHole(BlackHole *hole) {
  hole->listener = makeListener(1, &hole->port);
  EXPECT_GE(hole->listener, 0) << "makeListener: " << std::strerror(errno);
  if (hole->listener < 0) {
    return false;
  }

  const sockaddr_in server = loopbackAddress(hole->port);
  int inFlight = 0;
  for (int i = 0; i < 32; ++i) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0) << "socket: " << std::strerror(errno);
    if (fd < 0) {
      return false;
    }
    // Completed attempts occupy the accept queue; the rest stay in the SYN
    // queue. Both keep the queue full without accepting anyone.
    const int connectResult =
        ::connect(fd, reinterpret_cast<const sockaddr *>(&server),
                  sizeof(server));
    if (connectResult == 0 ||
        (connectResult < 0 &&
         (errno == EINPROGRESS || errno == EALREADY))) {
      ++inFlight;
    }
    hole->fillers.push_back(fd);
  }

  // Bounded wait for the loopback handshakes to settle (poll, not sleep): the
  // fillers that can complete become writable; the rest reach the poll timeout.
  std::vector<pollfd> pollFds;
  pollFds.reserve(hole->fillers.size());
  for (int fd : hole->fillers) {
    pollFds.push_back(pollfd{fd, POLLOUT, 0});
  }
  const int pollResult = ::poll(pollFds.data(), pollFds.size(), 100);
  EXPECT_GE(pollResult, 0) << "poll: " << std::strerror(errno);
  EXPECT_GT(inFlight, 0) << "no connection attempt is in flight";
  return true;
}

class ConnectorTest : public ::testing::Test {
protected:
  void SetUp() override { loop_ = thread_.startLoop(); }

  void TearDown() override {
    // The Connector belongs to the loop thread (its destructor asserts that),
    // so destroy it there before ~EventLoopThread stops the loop.
    if (loop_ != nullptr) {
      auto done = std::make_shared<std::promise<void>>();
      std::future<void> finished = done->get_future();
      loop_->runInLoop([this, done] {
        connector_.reset();
        done->set_value();
      });
      if (finished.wait_for(kWaitTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "the loop did not destroy the Connector in time";
      }
    }
  }

  // Creates the Connector on the loop thread. Does not start it, so a test can
  // time the attempt from its own `start()` call.
  void installConnector(uint16_t port, double timeoutSeconds,
                        std::shared_ptr<ConnectResult> result) {
    auto created = std::make_shared<std::promise<void>>();
    std::future<void> ready = created->get_future();
    loop_->runInLoop([this, port, timeoutSeconds, result, created] {
      connector_ =
          std::make_unique<Connector>(loop_, InetAddress(port, "127.0.0.1"));
      connector_->setConnectTimeout(timeoutSeconds);
      connector_->setNewConnectionCallback([result](int sockfd) {
        result->sockfd = sockfd;
        result->done.set_value();
      });
      connector_->setConnectErrorCallback([result](int savedErrno) {
        result->error = savedErrno;
        result->done.set_value();
      });
      created->set_value();
    });
    ASSERT_EQ(ready.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not create the Connector in time";
  }

  void runOnLoop(const std::function<void()> &fn) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    loop_->runInLoop([fn, done] {
      fn();
      done->set_value();
    });
    ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not run the task in time";
  }

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  std::unique_ptr<Connector> connector_; // owned by the loop thread
};

TEST_F(ConnectorTest, ConnectsToAListeningPortAndDeliversTheFd) {
  uint16_t port = 0;
  int listener = makeListener(1, &port);
  ASSERT_GE(listener, 0) << "makeListener: " << std::strerror(errno);

  auto result = std::make_shared<ConnectResult>();
  std::future<void> finished = result->done.get_future();
  installConnector(port, 1.0, result);
  runOnLoop([this] { connector_->start(); });

  ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
      << "no connect callback arrived";
  ASSERT_EQ(result->error, 0)
      << "connect failed: " << std::strerror(result->error);
  ASSERT_GE(result->sockfd, 0);

  // A descriptor alone is not proof: accept the connection and move a byte
  // through it, which only works on an established TCP connection.
  int peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
  ASSERT_GE(peer, 0) << "accept: " << std::strerror(errno);
  const char outgoing = 'x';
  ASSERT_EQ(::write(result->sockfd, &outgoing, 1), 1)
      << "write to the delivered fd: " << std::strerror(errno);
  char incoming = 0;
  ASSERT_EQ(::read(peer, &incoming, 1), 1)
      << "read on the accepted peer: " << std::strerror(errno);
  EXPECT_EQ(incoming, 'x');

  ::close(peer);
  ::close(result->sockfd);
  ::close(listener);
}

TEST_F(ConnectorTest, ClosedPortFailsImmediatelyWithEconnrefused) {
  uint16_t port = 0;
  int reserved = makeRefusingSocket(&port);
  ASSERT_GE(reserved, 0) << "makeRefusingSocket: " << std::strerror(errno);

  auto result = std::make_shared<ConnectResult>();
  std::future<void> finished = result->done.get_future();
  installConnector(port, 1.0, result);

  const auto startedAt = std::chrono::steady_clock::now();
  runOnLoop([this] { connector_->start(); });
  ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
      << "the refused connect never reported back";
  const auto elapsed = std::chrono::steady_clock::now() - startedAt;

  EXPECT_EQ(result->sockfd, -1);
  EXPECT_EQ(result->error, ECONNREFUSED)
      << "unexpected errno: " << std::strerror(result->error);
  EXPECT_LT(elapsed, 1s)
      << "a refused connection must fail immediately, not wait for the "
         "connect timeout";

  ::close(reserved);
}

TEST_F(ConnectorTest, UnansweredConnectFailsWithEtimedout) {
  if (readIntFile("/proc/sys/net/ipv4/tcp_abort_on_overflow") != 0) {
    GTEST_SKIP() << "the kernel aborts overflowing connections instead of "
                    "dropping their SYNs, so an accept-queue black hole cannot "
                    "be built deterministically";
  }

  BlackHole hole;
  ASSERT_TRUE(buildBlackHole(&hole));

  auto result = std::make_shared<ConnectResult>();
  std::future<void> finished = result->done.get_future();
  installConnector(hole.port, 0.3, result);

  const auto startedAt = std::chrono::steady_clock::now();
  runOnLoop([this] { connector_->start(); });
  ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
      << "the timed-out connect never reported back";
  const auto elapsed = std::chrono::steady_clock::now() - startedAt;

  EXPECT_EQ(result->sockfd, -1)
      << "connect unexpectedly succeeded; the accept queue was not full";
  EXPECT_EQ(result->error, ETIMEDOUT)
      << "unexpected errno: " << std::strerror(result->error);
  EXPECT_GE(elapsed, 250ms) << "the timeout fired before its deadline";
  EXPECT_LT(elapsed, kWaitTimeout);
}

TEST_F(ConnectorTest, AsynchronousFailureIsReadFromSoError) {
  // EPOLLOUT alone does not mean the connect succeeded: a failed handshake is
  // also reported as writability, with the cause in SO_ERROR. To exercise that
  // path, start a connect into the black hole and then close the listener; the
  // kernel resets the half-open connection and the client gets an error event.
  BlackHole hole;
  ASSERT_TRUE(buildBlackHole(&hole));

  auto result = std::make_shared<ConnectResult>();
  std::future<void> finished = result->done.get_future();
  // A long deadline: a failure arriving well before it can only have come from
  // SO_ERROR, not from the timeout. The reset is discovered on the client's
  // first SYN retransmission (~1s), so 3s leaves a clear margin on both sides.
  installConnector(hole.port, 3.0, result);
  runOnLoop([this] { connector_->start(); });

  // The attempt is in flight (its SYN was dropped because the accept queue is
  // full). Closing the listener resets it.
  ::close(hole.listener);
  hole.listener = -1;

  const auto startedAt = std::chrono::steady_clock::now();
  ASSERT_EQ(finished.wait_for(kWaitTimeout), std::future_status::ready)
      << "the asynchronously failed connect never reported back";
  const auto elapsed = std::chrono::steady_clock::now() - startedAt;

  EXPECT_EQ(result->sockfd, -1) << "a reset handshake must not be delivered";
  EXPECT_TRUE(result->error == ECONNREFUSED || result->error == ECONNRESET)
      << "unexpected errno: " << std::strerror(result->error);
  EXPECT_LT(elapsed, 2s)
      << "the result arrived at the deadline, not from SO_ERROR";
}

TEST_F(ConnectorTest, DestroyingWhileConnectingCancelsTheTimeout) {
  BlackHole hole;
  ASSERT_TRUE(buildBlackHole(&hole));

  auto result = std::make_shared<ConnectResult>();
  installConnector(hole.port, 0.2, result);
  runOnLoop([this] {
    connector_->start();  // arms the 0.2s timeout
    connector_.reset();   // destructor must cancel it: the callback has `this`
  });

  // Wait past the deadline. If the timer survived destruction, it fires into
  // freed memory here (ASan reports the use-after-free) before the sentinel.
  auto sentinel = std::make_shared<std::promise<void>>();
  std::future<void> fired = sentinel->get_future();
  loop_->runAfter(0.4, [sentinel] { sentinel->set_value(); });
  EXPECT_EQ(fired.wait_for(kWaitTimeout), std::future_status::ready)
      << "the loop did not survive the cancelled timeout";

  EXPECT_EQ(result->sockfd, -1);
  EXPECT_EQ(result->error, 0) << "destruction must not fire a callback";
}

TEST_F(ConnectorTest, RepeatedStartStopDoesNotLeakFileDescriptors) {
  // A large backlog keeps every attempt inside the "in progress" path instead
  // of being refused, so start() always creates a socket + Channel and stop()
  // always has something to tear down. stop() runs in the same loop-thread
  // task as start(), before the poller can report writability.
  uint16_t port = 0;
  int listener = makeListener(SOMAXCONN, &port);
  ASSERT_GE(listener, 0) << "makeListener: " << std::strerror(errno);

  auto result = std::make_shared<ConnectResult>();
  installConnector(port, 1.0, result);

  // Warm-up cycle: the first attempt allocates the Channel and any lazily
  // created loop state. Only the measured cycles are about leaks.
  runOnLoop([this] {
    connector_->start();
    connector_->stop();
  });

  const int before = countOpenFds();
  ASSERT_GE(before, 0) << "cannot read /proc/self/fd";

  runOnLoop([this] {
    for (int i = 0; i < 100; ++i) {
      connector_->start();
      connector_->stop();
    }
  });
  const int after = countOpenFds();

  EXPECT_EQ(after, before) << "start()/stop() leaked file descriptors";
  EXPECT_EQ(result->sockfd, -1) << "stop() must not deliver a connection";
  EXPECT_EQ(result->error, 0) << "stop() must not report a connect error";

  ::close(listener);
}

} // namespace
