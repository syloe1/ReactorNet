#include <gtest/gtest.h>

#include "Buffer.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "TcpClient.h"
#include "TcpServer.h"
#include "Transport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace reactornet;

// Tests for TcpClient (issue #25): the Connector + TcpConnection composition
// that mirrors TcpServer on the client side.
//
// One EventLoopThread hosts both the server and the client, so every callback
// runs on one thread and the test thread only observes events through
// condition variables. All waits have a deadline; nothing sleeps.
namespace {

using namespace std::chrono_literals;

// Loopback handshakes are microseconds; anything still pending after 5s is a
// hang and should fail the case rather than block the binary.
constexpr auto kWaitTimeout = 5s;

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

// A bound-but-never-listening loopback socket. connect() to its port is
// refused by the kernel, and no other process can take the port while the
// socket stays open, so the test cannot race with anything.
int makeRefusingSocket(uint16_t *port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
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

class TcpClientTest : public ::testing::Test {
protected:
  using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
  using TransportPtr = Transport::TransportPtr;

  void SetUp() override {
    loop_ = thread_.startLoop();
    startServer();
  }

  void TearDown() override {
    if (loop_ == nullptr) {
      return;
    }
    // Both objects belong to the loop thread; destroy them there before
    // ~EventLoopThread stops the loop. The client goes first so its close path
    // does not talk to an already-destroyed server.
    runOnLoop([this] { client_.reset(); });
    waitForLoopIdle();
    runOnLoop([this] { server_.reset(); });
  }

  // A port-0 TcpServer with an echo message callback. Its assigned port is
  // read on the loop thread after start(), so no test hard-codes one.
  void startServer() {
    auto ready = std::make_shared<std::promise<uint16_t>>();
    std::future<uint16_t> port = ready->get_future();
    loop_->runInLoop([this, ready] {
      server_ = std::make_unique<TcpServer>(
          loop_, InetAddress(0, "127.0.0.1"), "TcpClientTestServer");
      server_->setConnectionCallback([this](const TransportPtr &transport) {
        auto conn = std::dynamic_pointer_cast<TcpConnection>(transport);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (conn->connected()) {
            serverUp_ = true;
          } else {
            serverDown_ = true;
          }
        }
        // Loop-thread only; the test reads it solely inside runOnLoop tasks.
        if (conn->connected()) {
          serverConn_ = conn;
        } else {
          serverConn_.reset();
        }
        cv_.notify_all();
      });
      server_->setMessageCallback(
          [this](const TransportPtr &transport, Buffer *buf, Timestamp) {
            auto conn = std::dynamic_pointer_cast<TcpConnection>(transport);
            const std::string data = buf->retrieveAllAsString();
            {
              std::lock_guard<std::mutex> lock(mutex_);
              serverMessages_.push_back(data);
            }
            cv_.notify_all();
            conn->send(data); // echo
          });
      server_->start();
      ready->set_value(server_->listenPort());
    });
    ASSERT_EQ(port.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not start the server in time";
    port_ = port.get();
    ASSERT_NE(port_, 0u) << "the server did not report its listening port";
  }

  // Constructs the TcpClient on the loop thread, with callbacks that publish
  // their events to the test thread.
  void createClient(uint16_t port) {
    auto created = std::make_shared<std::promise<void>>();
    std::future<void> ready = created->get_future();
    loop_->runInLoop([this, port, created] {
      client_ = std::make_unique<TcpClient>(loop_,
                                            InetAddress(port, "127.0.0.1"),
                                            "TcpClientTestClient");
      client_->setConnectionCallback([this](const TransportPtr &transport) {
        auto conn = std::dynamic_pointer_cast<TcpConnection>(transport);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (conn->connected()) {
            clientUp_ = true;
          } else {
            clientDown_ = true;
          }
        }
        if (conn->connected()) {
          clientConn_ = conn;
        } else {
          clientConn_.reset();
        }
        cv_.notify_all();
      });
      client_->setMessageCallback(
          [this](const TransportPtr &, Buffer *buf, Timestamp) {
            {
              std::lock_guard<std::mutex> lock(mutex_);
              clientMessages_.push_back(buf->retrieveAllAsString());
            }
            cv_.notify_all();
          });
      client_->setConnectErrorCallback([this](int savedErrno) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          connectError_ = savedErrno;
          connectErrorSeen_ = true;
        }
        cv_.notify_all();
      });
      created->set_value();
    });
    ASSERT_EQ(ready.wait_for(kWaitTimeout), std::future_status::ready)
        << "the loop did not create the client in time";
  }

  bool waitFor(const std::function<bool()> &pred) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, kWaitTimeout, pred);
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

  // Two round trips through the loop queue. The first lets teardown tasks
  // queued from inside a connection's own dispatch run; the second proves the
  // first batch has finished. Only after that are the sockets really closed.
  void waitForLoopIdle() {
    runOnLoop([] {});
    runOnLoop([] {});
  }

  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  uint16_t port_ = 0;
  std::unique_ptr<TcpServer> server_;
  std::unique_ptr<TcpClient> client_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool serverUp_ = false;
  bool serverDown_ = false;
  bool clientUp_ = false;
  bool clientDown_ = false;
  bool connectErrorSeen_ = false;
  int connectError_ = 0;
  std::vector<std::string> serverMessages_;
  std::vector<std::string> clientMessages_;
  TcpConnectionPtr serverConn_; // loop-thread only, set by the callback
  TcpConnectionPtr clientConn_; // loop-thread only, set by the callback
};

TEST_F(TcpClientTest, ConnectsExchangesDataAndDisconnectsCleanly) {
  createClient(port_);
  runOnLoop([this] { client_->connect(); });
  ASSERT_TRUE(waitFor([this] { return clientUp_ && serverUp_; }))
      << "the client and the server did not both see the connection";

  const int fdsWhileConnected = countOpenFds();
  ASSERT_GE(fdsWhileConnected, 0);

  runOnLoop([this] {
    ASSERT_NE(clientConn_, nullptr);
    clientConn_->send("ping from client");
  });

  ASSERT_TRUE(waitFor([this] { return !clientMessages_.empty(); }))
      << "the echoed message never arrived";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EXPECT_EQ(clientMessages_.front(), "ping from client");
    EXPECT_EQ(serverMessages_.front(), "ping from client");
  }

  // An orderly client-side close: the server sees EOF and closes its end, the
  // client then observes the peer close.
  runOnLoop([this] { client_->disconnect(); });
  ASSERT_TRUE(waitFor([this] { return clientDown_ && serverDown_; }))
      << "the orderly close was not observed on both ends";

  waitForLoopIdle();
  EXPECT_EQ(countOpenFds(), fdsWhileConnected - 2)
      << "both established sockets must be closed after the disconnect";
  EXPECT_FALSE(connectErrorSeen_);
}

TEST_F(TcpClientTest, ServerInitiatedCloseReachesTheClientCallback) {
  createClient(port_);
  runOnLoop([this] { client_->connect(); });
  ASSERT_TRUE(waitFor([this] { return clientUp_ && serverUp_; }));

  const int fdsWhileConnected = countOpenFds();
  ASSERT_GE(fdsWhileConnected, 0);

  // The server closes first. The client must notice the peer close and invoke
  // its connection callback with connected() == false.
  runOnLoop([this] {
    ASSERT_NE(serverConn_, nullptr);
    serverConn_->forceClose();
  });

  ASSERT_TRUE(waitFor([this] { return clientDown_ && serverDown_; }))
      << "the server-initiated close was not observed on both ends";

  waitForLoopIdle();
  EXPECT_EQ(countOpenFds(), fdsWhileConnected - 2)
      << "both established sockets must be closed after the server close";
  EXPECT_FALSE(connectErrorSeen_)
      << "a close after a successful connect is not a connect error";
}

TEST_F(TcpClientTest, DestroyingAConnectedClientClosesTheSocket) {
  createClient(port_);
  runOnLoop([this] { client_->connect(); });
  ASSERT_TRUE(waitFor([this] { return clientUp_ && serverUp_; }));

  const int fdsWhileConnected = countOpenFds();
  ASSERT_GE(fdsWhileConnected, 0);

  // No disconnect(): destroying the client must tear down the connection.
  runOnLoop([this] { client_.reset(); });
  ASSERT_TRUE(waitFor([this] { return serverDown_; }))
      << "the server never saw the client socket disappear";

  waitForLoopIdle();
  EXPECT_EQ(countOpenFds(), fdsWhileConnected - 2)
      << "destruction must close both ends, not just forget the connection";
  EXPECT_TRUE(clientDown_)
      << "the client connection callback should see the teardown";
  EXPECT_FALSE(connectErrorSeen_);
}

TEST_F(TcpClientTest, DisconnectBeforeTheHandshakeCompletesCancelsTheAttempt) {
  createClient(port_);
  const int baselineFds = countOpenFds();
  ASSERT_GE(baselineFds, 0);

  // Both calls run in one loop turn. The Connector never reports completion
  // from inside connect(), so connection_ is still empty and disconnect()
  // cancels the in-flight attempt instead of closing a connection.
  runOnLoop([this] {
    client_->connect();
    client_->disconnect();
  });

  // Give a stray EPOLLOUT or the connect deadline a chance to misfire.
  auto sentinel = std::make_shared<std::promise<void>>();
  std::future<void> fired = sentinel->get_future();
  loop_->runAfter(0.2, [sentinel] { sentinel->set_value(); });
  ASSERT_EQ(fired.wait_for(kWaitTimeout), std::future_status::ready)
      << "the loop did not survive the canceled attempt";

  EXPECT_FALSE(clientUp_);
  EXPECT_FALSE(clientDown_);
  EXPECT_FALSE(connectErrorSeen_)
      << "a canceled attempt must not report a connect error";
  EXPECT_EQ(countOpenFds(), baselineFds)
      << "the canceled attempt leaked a socket";
}

TEST_F(TcpClientTest, ConnectFailureIsReportedWithTheRealErrno) {
  uint16_t refusedPort = 0;
  int reserved = makeRefusingSocket(&refusedPort);
  ASSERT_GE(reserved, 0) << "makeRefusingSocket: " << std::strerror(errno);

  createClient(refusedPort);
  runOnLoop([this] { client_->connect(); });

  ASSERT_TRUE(waitFor([this] { return connectErrorSeen_; }))
      << "the client never reported the refused connect";
  EXPECT_EQ(connectError_, ECONNREFUSED)
      << "unexpected errno: " << std::strerror(connectError_);
  EXPECT_FALSE(clientUp_) << "a failed connect must not report a connection";
  EXPECT_FALSE(clientDown_) << "a failed connect has no connection to close";

  runOnLoop([this] { client_.reset(); });
  waitForLoopIdle();
  ::close(reserved);
}

} // namespace
