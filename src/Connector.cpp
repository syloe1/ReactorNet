#include "Connector.h"
#include "Channel.h"
#include "EventLoop.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace reactornet {

Connector::Connector(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop), serverAddr_(serverAddr), state_(kDisconnected), sockfd_(-1),
      timeoutSeconds_(3.0) {}

Connector::~Connector() {
  // Destruction is a loop-thread operation: the Channel and the timeout timer
  // both belong to this loop.
  loop_->assertInLoopThread();
  // The timeout callback captures `this`; an armed timer left behind would
  // fire into a freed object.
  cancelTimeout();
  removeChannel();
  closeSockfd();
}

void Connector::setNewConnectionCallback(NewConnectionCallback cb) {
  newConnectionCallback_ = std::move(cb);
}

void Connector::setConnectErrorCallback(ConnectErrorCallback cb) {
  connectErrorCallback_ = std::move(cb);
}

void Connector::setConnectTimeout(double seconds) {
  timeoutSeconds_ = seconds;
}

void Connector::start() {
  // runInLoop, not queueInLoop: on the loop thread the attempt starts without
  // an extra wakeup; from another thread it is queued and the loop is woken.
  loop_->runInLoop([this] { startInLoop(); });
}

void Connector::stop() { loop_->runInLoop([this] { stopInLoop(); }); }

void Connector::startInLoop() {
  loop_->assertInLoopThread();
  if (state_ == kConnecting || state_ == kConnected) {
    return; // one attempt at a time; the fd is no longer ours after success
  }
  // kDisconnected (first attempt) or kFailed (manual retry).
  state_ = kConnecting;
  connect();
}

void Connector::stopInLoop() {
  loop_->assertInLoopThread();
  if (state_ == kConnecting) {
    state_ = kDisconnected;
    cancelTimeout();
    removeChannel();
    closeSockfd();
  } else if (state_ == kFailed) {
    // Failure already cleaned up; just return to the initial state so a later
    // start() is not blocked.
    state_ = kDisconnected;
  }
}

void Connector::connect() {
  loop_->assertInLoopThread();

  // SOCK_NONBLOCK is essential: a blocking connect() would freeze the whole
  // reactor for the kernel's SYN timeout (minutes), which is exactly what the
  // Connector's own deadline exists to bound.
  int sockfd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (sockfd < 0) {
    fail(errno);
    return;
  }
  sockfd_ = sockfd;

  channel_ = std::make_unique<Channel>(loop_, sockfd);
  channel_->setWriteCallback([this] { handleWrite(); });
  channel_->setErrorCallback([this] { handleError(); });

  int ret =
      ::connect(sockfd_, serverAddr_.getSockAddr(), serverAddr_.getSockLen());
  int savedErrno = (ret == 0) ? 0 : errno;

  // EINPROGRESS is the normal path for a non-blocking connect. EINTR means a
  // signal interrupted the wait, but Linux keeps completing the handshake in
  // the background, so it is handled the same way. EISCONN is the harmless
  // "already connected" answer after such an interruption.
  if (savedErrno == 0 || savedErrno == EINPROGRESS || savedErrno == EINTR ||
      savedErrno == EISCONN) {
    // Even an immediate ret == 0 goes through EPOLLOUT rather than reporting
    // success here. Writability is the single completion path, and both the
    // SO_ERROR check and the self-connect check have to run before the fd is
    // handed out. A connected socket is writable, so this costs one loop turn
    // and never waits for the network.
    channel_->enableWriting();
    if (timeoutSeconds_ > 0.0) {
      timeoutId_ = loop_->runAfter(timeoutSeconds_, [this] { handleTimeout(); });
    }
    return;
  }

  // A definite failure at syscall time (ECONNREFUSED, ENETUNREACH, ...).
  fail(savedErrno);
}

void Connector::handleWrite() {
  loop_->assertInLoopThread();
  if (state_ != kConnecting) {
    return;
  }

  // EPOLLOUT only says the socket became writable. For a connect that is
  // exactly what a completed handshake looks like, but also what a failed one
  // looks like (the error is delivered as a readiness event). SO_ERROR is the
  // only authoritative answer, and it also clears the pending error.
  int err = 0;
  socklen_t errLen = sizeof(err);
  if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
    err = errno;
  }
  if (err != 0) {
    fail(err);
    return;
  }

  if (isSelfConnect()) {
    // The kernel can pick the destination port as the local ephemeral port;
    // the resulting socket is connected to itself: it reports success but can
    // never carry data. Treat it as a failed attempt, not a connection.
    fail(ECONNREFUSED);
    return;
  }

  finishConnect();
}

void Connector::handleError() {
  loop_->assertInLoopThread();
  if (state_ != kConnecting) {
    return;
  }
  int err = 0;
  socklen_t errLen = sizeof(err);
  if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
    err = errno;
  }
  if (err == 0) {
    // EPOLLERR without a pending SO_ERROR would be a kernel inconsistency;
    // report a definite failure rather than a bogus success.
    err = ECONNABORTED;
  }
  fail(err);
}

void Connector::handleTimeout() {
  loop_->assertInLoopThread();
  if (state_ == kConnecting) {
    // Distinct from a refused connection: no answer ever arrived.
    fail(ETIMEDOUT);
  }
}

void Connector::finishConnect() {
  loop_->assertInLoopThread();
  cancelTimeout();
  removeChannel();

  // Hand the fd to the caller. The Connector must forget it before invoking the
  // callback: the callback may destroy this Connector.
  int sockfd = sockfd_;
  sockfd_ = -1;
  state_ = kConnected;

  if (newConnectionCallback_) {
    newConnectionCallback_(sockfd);
  } else {
    ::close(sockfd);
  }
}

void Connector::fail(int savedErrno) {
  loop_->assertInLoopThread();
  state_ = kFailed;
  cancelTimeout();
  removeChannel();
  closeSockfd();

  // Last, so a callback that immediately calls start() finds clean state.
  if (connectErrorCallback_) {
    connectErrorCallback_(savedErrno);
  }
}

void Connector::removeChannel() {
  if (!channel_) {
    return;
  }
  channel_->disableAll();
  channel_->remove();

  // This can run from inside the Channel's own event dispatch (handleWrite /
  // handleError). ~Channel asserts that no dispatch is in progress, so the
  // object must outlive the callback: hand ownership to a queued task, which
  // runs after the current dispatch returns.
  std::shared_ptr<Channel> doomed(std::move(channel_));
  loop_->queueInLoop([doomed]() {});
}

void Connector::closeSockfd() {
  if (sockfd_ >= 0) {
    ::close(sockfd_);
    sockfd_ = -1;
  }
}

void Connector::cancelTimeout() {
  if (timeoutId_.timer() != nullptr) {
    loop_->cancelTimer(timeoutId_);
    timeoutId_ = TimerId();
  }
}

bool Connector::isSelfConnect() const {
  sockaddr_in local;
  sockaddr_in peer;
  socklen_t localLen = sizeof(local);
  socklen_t peerLen = sizeof(peer);
  std::memset(&local, 0, sizeof(local));
  std::memset(&peer, 0, sizeof(peer));
  if (::getsockname(sockfd_, reinterpret_cast<sockaddr *>(&local), &localLen) <
      0) {
    return false;
  }
  if (::getpeername(sockfd_, reinterpret_cast<sockaddr *>(&peer), &peerLen) <
      0) {
    return false;
  }
  return local.sin_family == peer.sin_family &&
         local.sin_port == peer.sin_port &&
         local.sin_addr.s_addr == peer.sin_addr.s_addr;
}

} // namespace reactornet
