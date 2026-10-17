#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "Socket.h"

using namespace reactornet;

// Unit tests for Socket::read / write / readv.
//
// These three wrappers are pure forwarders to the corresponding system calls,
// but they are the seam TcpConnection and Buffer use to stop touching raw fds.
// #59 introduced them with no production caller until #62, so without these
// tests they would be untested code.
//
// As in buffer_test.cpp, socketpair(AF_UNIX, SOCK_STREAM) supplies a real
// connected stream: short reads/writes and EOF behave like TCP without a
// filesystem or network dependency. The sockets are blocking, so no test needs
// a sleep or a timeout.
namespace {

class SocketTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0)
        << "socketpair: " << std::strerror(errno);
    // Writing to a socket whose peer has closed raises SIGPIPE, which would
    // kill the test process before errno can be inspected. EventLoop installs
    // the same handler for the library; the tests restore it afterwards.
    oldSigpipe_ = ::signal(SIGPIPE, SIG_IGN);
  }

  void TearDown() override {
    ::signal(SIGPIPE, oldSigpipe_);
    for (int &fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  // Transfers ownership of fds_[0] to the returned Socket so TearDown does not
  // close the same fd twice.
  Socket takeSocket() {
    const int fd = fds_[0];
    fds_[0] = -1;
    return Socket(fd);
  }

  void closePeer() {
    ASSERT_GE(fds_[1], 0);
    ASSERT_EQ(::close(fds_[1]), 0) << std::strerror(errno);
    fds_[1] = -1;
  }

  void peerWrite(const std::string &data) {
    size_t written = 0;
    while (written < data.size()) {
      const ssize_t n =
          ::write(fds_[1], data.data() + written, data.size() - written);
      ASSERT_GT(n, 0) << "peer write: " << std::strerror(errno);
      written += static_cast<size_t>(n);
    }
  }

  int fds_[2] = {-1, -1};
  using SignalHandler = void (*)(int);
  SignalHandler oldSigpipe_ = SIG_DFL;
};

TEST_F(SocketTest, WriteSendsAllBytesToPeer) {
  Socket sock = takeSocket();
  const std::string payload = "hello from Socket::write";

  const ssize_t n = sock.write(payload.data(), payload.size());
  ASSERT_EQ(n, static_cast<ssize_t>(payload.size()))
      << "write: " << std::strerror(errno);

  std::string received(payload.size(), '\0');
  size_t got = 0;
  while (got < received.size()) {
    const ssize_t r = ::read(fds_[1], &received[got], received.size() - got);
    ASSERT_GT(r, 0) << "peer read: " << std::strerror(errno);
    got += static_cast<size_t>(r);
  }
  EXPECT_EQ(received, payload);
}

// Once the peer is gone, write must surface the error instead of pretending to
// succeed. The assertion accepts ECONNRESET as well because a stream socket is
// allowed to report either when the peer disappears.
TEST_F(SocketTest, WriteToClosedPeerReturnsMinusOneAndErrno) {
  Socket sock = takeSocket();
  closePeer();

  errno = 0;
  const char byte = 'x';
  const ssize_t n = sock.write(&byte, 1);

  EXPECT_EQ(n, -1);
  EXPECT_TRUE(errno == EPIPE || errno == ECONNRESET)
      << "unexpected errno: " << std::strerror(errno);
}

TEST_F(SocketTest, ReadReturnsBytesWrittenByPeer) {
  Socket sock = takeSocket();
  const std::string payload = "bytes in, bytes out";
  peerWrite(payload);

  std::string buffer(payload.size(), '\0');
  size_t got = 0;
  while (got < buffer.size()) {
    const ssize_t n = sock.read(&buffer[got], buffer.size() - got);
    ASSERT_GT(n, 0) << "read: " << std::strerror(errno);
    got += static_cast<size_t>(n);
  }
  EXPECT_EQ(buffer, payload);
}

// EOF is the stream's end-of-file signal; TcpConnection relies on exactly this
// return value to notice that the peer closed its write side.
TEST_F(SocketTest, ReadReturnsZeroAfterPeerCloses) {
  Socket sock = takeSocket();
  closePeer();

  char buffer[16];
  EXPECT_EQ(sock.read(buffer, sizeof(buffer)), 0);
}

TEST_F(SocketTest, ReadvScattersAcrossTwoSegments) {
  Socket sock = takeSocket();
  peerWrite("hello world");

  char first[5];
  char second[6];
  struct iovec iov[2];
  iov[0].iov_base = first;
  iov[0].iov_len = sizeof(first);
  iov[1].iov_base = second;
  iov[1].iov_len = sizeof(second);

  const ssize_t n = sock.readv(iov, 2);
  ASSERT_EQ(n, static_cast<ssize_t>(sizeof(first) + sizeof(second)))
      << "readv: " << std::strerror(errno);
  EXPECT_EQ(std::string(first, sizeof(first)), "hello");
  EXPECT_EQ(std::string(second, sizeof(second)), " world");
}

// iovcnt == 1 is the degenerate path Buffer uses when only one writable region
// is available.
TEST_F(SocketTest, ReadvSingleIovec) {
  Socket sock = takeSocket();
  const std::string payload = "single segment";
  peerWrite(payload);

  std::string buffer(payload.size(), '\0');
  struct iovec iov;
  iov.iov_base = &buffer[0];
  iov.iov_len = buffer.size();

  const ssize_t n = sock.readv(&iov, 1);
  ASSERT_EQ(n, static_cast<ssize_t>(payload.size()))
      << "readv: " << std::strerror(errno);
  EXPECT_EQ(buffer, payload);
}

} // namespace
