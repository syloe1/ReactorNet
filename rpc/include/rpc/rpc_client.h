#pragma once

#include "InetAddress.h"
#include "Transport.h"
#include "noncopyable.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>

namespace reactornet {
class EventLoop;
class TcpClient;
} // namespace reactornet

namespace reactornet::rpc {

// Client side of the v0.1 RPC stack: TcpClient + FrameReader + Codec.
//
// v0.1 deliberately allows exactly one in-flight request. That removes the
// need for a pending-request table and lets the request/response loop be
// tested end to end; concurrent correlation (M3) is then an additive change
// rather than a rewrite.
//
// Threading:
//   - connect() and Call() are blocking and MUST NOT be called on the loop
//     thread. Call() in particular would deadlock: the response callback runs
//     on that same thread and could never resolve the promise it is waiting
//     for. Both assert rather than hang.
//   - disconnect() is safe from any thread (TcpClient hops onto the loop).
//   - Like TcpClient, the object must be destroyed on the loop thread, and
//     only after every Call() has returned.
//
// Lifetime of a call: Call() publishes one promise, posts the frame to the
// loop, and blocks on its future. The loop thread sends the frame on connect
// or resolves the promise immediately; a response, connect error or peer close
// resolves it from the loop thread. v0.1 has no response timeout (#39): if the
// peer stays connected and never answers, Call() waits just as long.
//
// Failure semantics before #39: transport problems (not connected, peer
// closed, malformed response) surface as Response{status = kInternalError,
// payload = ""} rather than an exception or a hang. Application-level misses
// still carry the server's own status (kUnknownService / kUnknownMethod).
class RpcClient : noncopyable {
public:
  RpcClient(EventLoop *loop, const InetAddress &serverAddr);
  ~RpcClient();

  // Starts one connection attempt and blocks until the handshake completes or
  // fails. The loop must already be running (e.g. an EventLoopThread).
  // A no-op when already connected. Failure is not thrown: a later Call()
  // reports kInternalError. A client is single-session: once a successful
  // connection has ended, calling connect() again asserts rather than waiting
  // for a handshake TcpClient will never start.
  void connect();

  // Orderly close: pending output is flushed, then the write side is shut
  // down. A subsequent Call() returns kInternalError; there is no reconnect.
  void disconnect();

  // Sends one request and blocks until its response arrives or the connection
  // fails. Must be called from a thread other than the loop thread; at most one
  // Call may be outstanding. An invalid Request (empty service/method, body
  // over the protocol limit) and a disconnected client both return
  // kInternalError.
  Response Call(const Request &request);

private:
  using TransportPtr = Transport::TransportPtr;

  void onConnection(const TransportPtr &transport);
  void onMessage(const TransportPtr &transport, Buffer *buf,
                 Timestamp receiveTime);
  void onConnectError(int savedErrno);

  // Loop-thread half of Call(): checks the connection, encodes the frame and
  // sends it, or resolves the waiting promise with an error.
  void sendRequestInLoop(const Request &request, uint32_t requestId);

  // Loop-thread helper: take the pending promise (if any) and fulfil it.
  void resolveCall(const Response &response);
  // Loop-thread helper: fail the pending call and tear the connection down.
  void failConnection();

  EventLoop *const loop_;
  std::unique_ptr<TcpClient> client_;

  RawCodec codec_;
  FrameReader frameReader_;
  TransportPtr connection_; // loop-thread only

  std::atomic<bool> connected_{false};
  std::atomic<bool> everConnected_{false};
  std::atomic<bool> callInFlight_{false};
  std::atomic<uint32_t> nextRequestId_{1};

  // Shared between the calling thread and the loop thread.
  std::mutex mutex_;
  std::shared_ptr<std::promise<Response>> pendingCall_;
  uint32_t pendingRequestId_ = 0;
  std::shared_ptr<std::promise<bool>> pendingConnect_;
};

} // namespace reactornet::rpc
