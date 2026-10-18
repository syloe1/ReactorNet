#pragma once

#include "InetAddress.h"
#include "Transport.h"
#include "noncopyable.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace reactornet {
class EventLoop;
class TcpClient;
} // namespace reactornet

namespace reactornet::rpc {

// Client side of the v0.1 RPC stack: TcpClient + FrameReader + Codec.
//
// Requests are correlated by request id: every request gets a non-zero id and
// its response is looked up in the loop-thread-owned pending table. Responses
// may therefore arrive in any order (the server's work takes different times),
// and several calls may be in flight at once.
//
// Threading:
//   - Call() is blocking and MUST NOT be called on the loop thread. It would
//     deadlock: the response callback runs on that same thread and could never
//     resolve the promise it is waiting for. It asserts rather than hangs.
//     Call() may be called from several threads concurrently.
//   - AsyncCall() and CallAsync() may be called from any thread, including the
//     loop thread. They only post to the loop and never wait for the response.
//   - Every ResponseCallback runs on the loop thread, including the callback
//     of a call that fails immediately (not connected, invalid request). A
//     callback must therefore not block, and must not call the synchronous
//     Call(): the response it waits for is delivered by that same thread.
//     Issuing another AsyncCall() from a callback is fine.
//   - The pending table is only ever touched on the loop thread, so it needs no
//     lock. The request-id counter is atomic for the same reason it is exposed
//     across threads.
//   - disconnect() is safe from any thread (TcpClient hops onto the loop).
//   - Like TcpClient, the object must be destroyed on the loop thread, and
//     only after every Call() has returned and every callback has run.
//
// Lifetime of a call: the request is posted to the loop, which assigns an id,
// records the pending call and sends the frame; a matching response, connect
// error or peer close resolves it. Call() is just CallAsync().get() on a
// non-loop thread. v0.1 has no response timeout (#38): if the peer stays
// connected and never answers, the future (and Call()) waits just as long.
// C++17 futures have no continuation, so CallAsync() only offers get()/wait();
// use AsyncCall() when the completion should run without a waiting thread.
//
// Failure semantics before #39: transport problems (not connected, peer
// closed, malformed response) surface as Response{status = kInternalError,
// payload = ""} rather than an exception or a hang. Application-level misses
// still carry the server's own status (kUnknownService / kUnknownMethod).
//
// A response whose id is not in the pending table is a protocol-level fault:
// the two ends no longer agree about the stream, so the connection is closed
// and every pending call fails with kInternalError (RFC-0001; #34).
class RpcClient : noncopyable {
public:
  // Always invoked on the loop thread. See the threading contract above.
  using ResponseCallback = std::function<void(Response)>;

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
  // down. Outstanding calls fail with kInternalError; there is no reconnect.
  void disconnect();

  // Sends one request and blocks until its response arrives or the connection
  // fails. Must be called from a thread other than the loop thread; any
  // number of threads may call it concurrently. An invalid Request (empty
  // service/method, body over the protocol limit) and a disconnected client
  // both return kInternalError.
  Response Call(const Request &request);

  // Non-blocking form of Call(): posts the request and returns immediately.
  // The callback runs on the loop thread with the response, or with
  // kInternalError if the call cannot be started. `callback` must not be
  // empty. The client must stay alive until the callback has run.
  void AsyncCall(const Request &request, ResponseCallback callback);

  // Future form of AsyncCall(). The future must be waited on from a thread
  // other than the loop thread, and while the client is alive. Like Call(), a
  // call that cannot be started yields kInternalError.
  std::future<Response> CallAsync(const Request &request);

private:
  using TransportPtr = Transport::TransportPtr;
  using Completion = ResponseCallback;

  // One in-flight request. Owned by the loop thread, like the map itself.
  // #38 will add the deadline and timer needed for timeouts.
  struct PendingCall {
    uint32_t id;
    Completion complete; // must run on the loop thread
  };

  void onConnection(const TransportPtr &transport);
  void onMessage(const TransportPtr &transport, Buffer *buf,
                 Timestamp receiveTime);
  void onConnectError(int savedErrno);

  // Loop-thread half of Call(): validates the connection, allocates an id,
  // records the pending call and sends the frame. Any failure is reported
  // through `complete` on this same thread.
  void startCallInLoop(const Request &request, Completion complete);

  uint32_t allocateRequestId();
  void failAllPending();
  void failConnection();

  EventLoop *const loop_;
  std::unique_ptr<TcpClient> client_;

  RawCodec codec_;
  FrameReader frameReader_;
  TransportPtr connection_; // loop-thread only

  // Loop-thread owned. No lock: every access is on the connection's loop.
  std::unordered_map<uint32_t, PendingCall> pendings_;
  std::atomic<uint32_t> nextRequestId_{1}; // 0 is reserved as invalid

  std::atomic<bool> connected_{false};
  std::atomic<bool> everConnected_{false};

  // connect() blocks a business thread while the loop completes the handshake,
  // so this one promise is genuinely shared across threads.
  std::mutex mutex_;
  std::shared_ptr<std::promise<bool>> pendingConnect_;
};

} // namespace reactornet::rpc
