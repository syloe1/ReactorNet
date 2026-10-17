#pragma once

#include "InetAddress.h"
#include "Timer.h"
#include "noncopyable.h"
#include <functional>
#include <memory>

namespace reactornet {

class Channel;
class EventLoop;

// Client-side connection establishment: exactly one non-blocking connect
// attempt per start().
//
// State machine: kDisconnected -> kConnecting -> kConnected / kFailed.
// No automatic retry, no backoff, no concurrent attempts — those belong to a
// layer above this one.
//
// Threading: the object belongs to the loop thread. start() and stop() are
// safe to call from any thread (they hop onto the loop), but the Connector
// itself must be destroyed on the loop thread, or after the loop has stopped:
// a timeout callback and a Channel refer to it / its fd.
class Connector : noncopyable {
public:
  // Called on the loop thread when the TCP handshake has completed and
  // getsockopt(SO_ERROR) says so. Ownership of sockfd passes to the callback.
  using NewConnectionCallback = std::function<void(int sockfd)>;

  // Called on the loop thread when the attempt failed. savedErrno is the real
  // cause (ECONNREFUSED, ENETUNREACH, ...), never 0. A connection that got no
  // answer at all is reported as ETIMEDOUT, so callers can distinguish "the
  // peer said no" from "the peer never answered".
  using ConnectErrorCallback = std::function<void(int savedErrno)>;

  Connector(EventLoop *loop, const InetAddress &serverAddr);
  ~Connector();

  void setNewConnectionCallback(NewConnectionCallback cb);
  void setConnectErrorCallback(ConnectErrorCallback cb);

  // Per-attempt deadline, in seconds; <= 0 disables it. Defaults to 3s. Set it
  // before start(); changing it while a connect is in flight has no effect on
  // that attempt.
  void setConnectTimeout(double seconds);

  // Begin one attempt. Ignored while connecting or after the fd has been
  // delivered; allowed again after a failure (a manual retry).
  void start();

  // Cancel an in-flight attempt: removes the channel, closes the socket and
  // cancels the timeout. No callback fires. After success this is a no-op —
  // the fd already belongs to the caller.
  void stop();

private:
  enum StateE { kDisconnected, kConnecting, kConnected, kFailed };

  void startInLoop();
  void stopInLoop();
  void connect();
  void handleWrite();
  void handleError();
  void handleTimeout();
  void finishConnect();
  void fail(int savedErrno);
  void removeChannel();
  void closeSockfd();
  void cancelTimeout();
  bool isSelfConnect() const;

  EventLoop *loop_;
  const InetAddress serverAddr_;
  StateE state_;
  std::unique_ptr<Channel> channel_;
  int sockfd_;
  TimerId timeoutId_; // valid only while kConnecting
  double timeoutSeconds_;
  NewConnectionCallback newConnectionCallback_;
  ConnectErrorCallback connectErrorCallback_;
};

} // namespace reactornet
