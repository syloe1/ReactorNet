#pragma once

#include "InetAddress.h"
#include "Timer.h"
#include "Transport.h"
#include "noncopyable.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
// records the pending call and sends the frame; a matching response, deadline,
// connect error or peer close resolves it. Call() is just CallAsync().get() on
// a non-loop thread. C++17 futures have no continuation, so CallAsync() only
// offers get()/wait(); use AsyncCall() when the completion should run without a
// waiting thread.
//
// Timeouts (#38): a positive timeoutSeconds arms one timer per in-flight call;
// at the deadline the call completes with kTimeout and its pending entry is
// removed. A timeout of 0 or less means "no deadline", which is the v0.1
// behaviour. One timer per call is O(log n) in the timer heap and is sized for
// hundreds to a few thousand in-flight calls; at a much larger scale switch to
// a fixed-tick scan over the pending table (gRPC's timeoutChecker does this).
//
// Late responses: after a timeout the id is remembered in a bounded FIFO
// (kMaxRememberedTimeouts). A response carrying such an id is silently
// discarded — it is late, not a protocol violation. Any other unknown id is
// still a protocol-level fault: the two ends no longer agree about the stream,
// so the connection is closed and every pending call fails with
// kConnectionClosed (RFC-0001; #34). Ids in the remembered set are not reused,
// so a late response can never be delivered to an unrelated call.
//
// Failure semantics (#39): every failure is a Response with an empty payload
// and a status that says whose fault it was —
//   - server answers: the server's own status (kUnknownService,
//     kUnknownMethod, kInternalError, ...) is passed through unchanged;
//   - transport: no connection, peer closed, or malformed response stream →
//     kConnectionClosed (the request may never have reached the server);
//   - caller: the request was rejected before sending, e.g. an empty service
//     or method name or an over-limit body → kBadRequest (the connection is
//     untouched);
//   - deadline: kTimeout (#38).
// No failure path throws or hangs. A handler that throws on the server is
// caught there and arrives as kInternalError; the server and the connection
// survive it.
class RpcClient : noncopyable {
public:
  // Always invoked on the loop thread. See the threading contract above.
  using ResponseCallback = std::function<void(Response)>;

  RpcClient(EventLoop *loop, const InetAddress &serverAddr);
  ~RpcClient();

  // Starts one connection attempt and blocks until the handshake completes or
  // fails. The loop must already be running (e.g. an EventLoopThread).
  // A no-op when already connected. Failure is not thrown: a later Call()
  // reports kConnectionClosed. A client is single-session: once a successful
  // connection has ended, calling connect() again asserts rather than waiting
  // for a handshake TcpClient will never start.
  void connect();

  // Orderly close: pending output is flushed, then the write side is shut
  // down. Outstanding calls fail with kConnectionClosed; there is no
  // reconnect.
  void disconnect();

  // Sends one request and blocks until its response arrives or the connection
  // fails. Must be called from a thread other than the loop thread; any
  // number of threads may call it concurrently. An invalid Request (empty
  // service/method, body over the protocol limit) returns kBadRequest; a
  // disconnected or never-connected client returns kConnectionClosed.
  Response Call(const Request &request);

  // Same as above but gives up after timeoutSeconds and returns kTimeout.
  // timeoutSeconds <= 0 means no deadline.
  Response Call(const Request &request, double timeoutSeconds);

  // Non-blocking form of Call(): posts the request and returns immediately.
  // The callback runs on the loop thread with the response, or with
  // kConnectionClosed / kBadRequest if the call cannot be started (kTimeout
  // on expiry). `callback` must not be empty. The client must stay alive
  // until the callback has run.
  void AsyncCall(const Request &request, ResponseCallback callback);

  // Timeout variant of AsyncCall(); timeoutSeconds <= 0 means no deadline.
  void AsyncCall(const Request &request, ResponseCallback callback,
                 double timeoutSeconds);

  // Future form of AsyncCall(). The future must be waited on from a thread
  // other than the loop thread, and while the client is alive. Like Call(), a
  // call that cannot be started yields kConnectionClosed or kBadRequest;
  // expiry yields kTimeout.
  std::future<Response> CallAsync(const Request &request);

  // Timeout variant of CallAsync(); timeoutSeconds <= 0 means no deadline.
  std::future<Response> CallAsync(const Request &request,
                                  double timeoutSeconds);

private:
  using TransportPtr = Transport::TransportPtr;
  using Completion = ResponseCallback;

  static constexpr size_t kMaxRememberedTimeouts = 1024;

  // One in-flight request. Owned by the loop thread, like the map itself.
  struct PendingCall {
    uint32_t id;
    Completion complete; // must run on the loop thread
    TimerId timer;       // empty when the call has no deadline
  };

  void onConnection(const TransportPtr &transport);
  void onMessage(const TransportPtr &transport, Buffer *buf,
                 Timestamp receiveTime);
  void onConnectError(int savedErrno);

  // Loop-thread half of Call(): validates the connection, allocates an id,
  // records the pending call (and its deadline) and sends the frame. Any
  // failure is reported through `complete` on this same thread.
  void startCallInLoop(const Request &request, Completion complete,
                       double timeoutSeconds);

  uint32_t allocateRequestId();
  void onTimeout(uint32_t id);
  void rememberTimedOut(uint32_t id);
  bool wasTimedOut(uint32_t id) const;
  void failAllPending();
  void failConnection();

  EventLoop *const loop_;
  std::unique_ptr<TcpClient> client_;

  RawCodec codec_;
  FrameReader frameReader_;
  TransportPtr connection_; // loop-thread only

  // Loop-thread owned. No lock: every access is on the connection's loop.
  std::unordered_map<uint32_t, PendingCall> pendings_;
  // Bounded FIFO of ids that timed out, so their late responses can be told
  // apart from genuinely unknown ids. Both containers are loop-thread only.
  std::deque<uint32_t> timedOutIds_;
  std::unordered_set<uint32_t> timedOutIdSet_;
  std::atomic<uint32_t> nextRequestId_{1}; // 0 is reserved as invalid

  std::atomic<bool> connected_{false};
  std::atomic<bool> everConnected_{false};

  // connect() blocks a business thread while the loop completes the handshake,
  // so this one promise is genuinely shared across threads.
  std::mutex mutex_;
  std::shared_ptr<std::promise<bool>> pendingConnect_;
};

} // namespace reactornet::rpc
