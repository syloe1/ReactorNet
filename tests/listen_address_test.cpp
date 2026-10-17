#include <gtest/gtest.h>

#include "Acceptor.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Socket.h"
#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace reactornet;

// Tests for the listen-address facade (issue #22).
//
// The property these APIs exist for is "bind port 0, then learn the port the
// kernel assigned". A number alone cannot prove it: the reported port must have
// a real listener behind it, so the server case connects to it with a plain
// POSIX socket. No port is ever hardcoded here — a fixed port is exactly what
// makes two ctest runs on one machine collide.
//
// Two layers are covered separately:
//   - Socket::getLocalAddr is pure getsockname and needs no loop.
//   - Acceptor/TcpServer are loop-thread objects, so this fixture owns a thread
//     that constructs the server, and the test thread asks that loop for the
//     address instead of reading it directly.
namespace {

using namespace std::chrono_literals;

// Every wait is bounded. Real latencies here are microseconds, so a wait still
// unsatisfied after 3s is a hang and must fail the case, not block it.
constexpr auto kWaitTimeout = 3s;

// --- Socket::getLocalAddr ---------------------------------------------------

TEST(ListenAddressSocketTest, ReportsKernelAssignedPortForPortZero) {
  Socket socket;
  socket.bind(InetAddress(0, "127.0.0.1"));

  InetAddress actual;
  ASSERT_TRUE(socket.getLocalAddr(&actual))
      << "getsockname on a bound socket must succeed";
  EXPECT_NE(actual.toPort(), 0)
      << "bind(port 0) must have been assigned a real ephemeral port";
  EXPECT_EQ(actual.toIp(), "127.0.0.1");
}

TEST(ListenAddressSocketTest, ReportsTheExplicitPortUnchanged) {
  // Both sockets set SO_REUSEPORT, so the second can bind the same address the
  // first still holds. The alternative — close the probe, then rebind its port —
  // has a TOCTOU window where another process can take the port, which is the
  // very flakiness this issue is trying to remove. The port itself is not
  // hardcoded for the same reason.
  Socket probe;
  probe.setReusePort(true);
  probe.bind(InetAddress(0, "127.0.0.1"));
  InetAddress assigned;
  ASSERT_TRUE(probe.getLocalAddr(&assigned));
  const uint16_t requested = assigned.toPort();
  ASSERT_NE(requested, 0);

  Socket socket;
  socket.setReusePort(true);
  socket.bind(InetAddress(requested, "127.0.0.1"));

  InetAddress actual;
  ASSERT_TRUE(socket.getLocalAddr(&actual));
  EXPECT_EQ(actual.toPort(), requested)
      << "an explicitly requested port must come back unchanged";
  EXPECT_EQ(actual.toIp(), "127.0.0.1");
}

// --- Acceptor / TcpServer ---------------------------------------------------

class ListenAddressServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto started = std::make_shared<std::promise<void>>();
    std::future<void> ready = started->get_future();

    thread_ = std::thread([this, started] {
      // Both objects belong to this thread: the Acceptor binds and registers a
      // Channel, and ~TcpServer asserts loop-thread affinity. The test thread
      // never touches them directly; it posts functors through onLoopAddress().
      EventLoop loop;
      loop_ = &loop;
      TcpServer server(&loop, InetAddress(0, "127.0.0.1"),
                       "ListenAddressTest");
      server_ = &server;
      server.start();
      // Published only after start(), so the test thread's first functor finds
      // a listening server whose address is final.
      started->set_value();
      loop.loop();
      // server is destroyed here, on the loop thread, before the EventLoop.
    });

    ASSERT_EQ(ready.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop thread never started the server";
  }

  void TearDown() override {
    // Nothing in these cases stops the loop, so quit() always reaches a live
    // EventLoop; join then waits for the loop thread to unwind the server and
    // the loop before the fixture's memory goes away.
    if (loop_ != nullptr) {
      loop_->quit();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // listenAddress() is loop-thread-only by contract, so a test must ask the
  // loop. On timeout this records a failure and returns a default address, so
  // the caller's assertion fails with a useful message instead of hanging.
  InetAddress onLoopAddress(std::function<InetAddress()> fn) {
    auto answer = std::make_shared<std::promise<InetAddress>>();
    std::future<InetAddress> result = answer->get_future();
    loop_->runInLoop([answer, fn = std::move(fn)] { answer->set_value(fn()); });
    if (result.wait_for(kWaitTimeout) != std::future_status::ready) {
      ADD_FAILURE() << "the base loop did not answer in time";
      return InetAddress();
    }
    return result.get();
  }

  EventLoop *loop_ = nullptr;
  TcpServer *server_ = nullptr;
  std::thread thread_;
};

TEST_F(ListenAddressServerTest, ReportsKernelAssignedPortAndListensOnIt) {
  const InetAddress address =
      onLoopAddress([this] { return server_->listenAddress(); });

  ASSERT_NE(address.toPort(), 0)
      << "a server constructed with port 0 must report the assigned port";
  EXPECT_EQ(address.toIp(), "127.0.0.1");

  // The number alone would pass even if the server were listening on a
  // different port. A successful connect proves there is a listener behind the
  // address this API reported.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << "socket(): " << std::strerror(errno);
  const int ret = ::connect(fd, address.getSockAddr(), address.getSockLen());
  const int savedErrno = errno;
  ::close(fd);
  ASSERT_EQ(ret, 0) << "connect() to the reported port failed: "
                    << std::strerror(savedErrno);
}

TEST_F(ListenAddressServerTest, ReportsAnExplicitlyRequestedPort) {
  const InetAddress first =
      onLoopAddress([this] { return server_->listenAddress(); });
  ASSERT_NE(first.toPort(), 0);

  // Both acceptors enable SO_REUSEPORT, so a second server may bind the address
  // the fixture's server already holds. That makes "an explicit port is
  // returned unchanged" testable without a probe-then-rebind race.
  const InetAddress explicitAddress = onLoopAddress([this, first] {
    TcpServer second(loop_, InetAddress(first.toPort(), "127.0.0.1"),
                     "ExplicitPortTest");
    second.start();
    return second.listenAddress();
  });

  EXPECT_EQ(explicitAddress.toPort(), first.toPort())
      << "an explicitly requested port must come back unchanged";
  EXPECT_EQ(explicitAddress.toIp(), "127.0.0.1");
}

TEST_F(ListenAddressServerTest, AcceptorKnowsItsAddressBeforeListen) {
  // Defined semantics for the pre-listen window: the constructor binds, so the
  // kernel-assigned port is already final even though listen() has not run.
  // The Acceptor is constructed and destroyed inside the loop functor, on its
  // own thread, exactly like a real one.
  const InetAddress address = onLoopAddress([this] {
    Acceptor acceptor(loop_, InetAddress(0, "127.0.0.1"));
    EXPECT_FALSE(acceptor.listening());
    return acceptor.listenAddress();
  });

  EXPECT_NE(address.toPort(), 0)
      << "bind(port 0) assigns the port before listen()";
  EXPECT_EQ(address.toIp(), "127.0.0.1");
}

} // namespace
