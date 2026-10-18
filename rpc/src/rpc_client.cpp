#include "rpc/rpc_client.h"

#include "EventLoop.h"
#include "TcpClient.h"

#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace reactornet::rpc {

namespace {

// Failure results produced by the client itself. They never carry a server
// status: the server never saw the request, or the stream was already broken.
Response connectionClosedResponse() {
  return Response{StatusCode::kConnectionClosed, ""};
}

Response badRequestResponse() { return Response{StatusCode::kBadRequest, ""}; }

Response timeoutResponse() { return Response{StatusCode::kTimeout, ""}; }

} // namespace

RpcClient::RpcClient(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop),
      client_(std::make_unique<TcpClient>(loop, serverAddr, "RpcClient")) {
  client_->setConnectionCallback(
      [this](const TransportPtr &transport) { onConnection(transport); });
  client_->setMessageCallback([this](const TransportPtr &transport,
                                     Buffer *buf, Timestamp receiveTime) {
    onMessage(transport, buf, receiveTime);
  });
  client_->setConnectErrorCallback(
      [this](int savedErrno) { onConnectError(savedErrno); });
}

RpcClient::~RpcClient() {
  // TcpClient (and the Connector inside it) is loop-thread owned.
  loop_->assertInLoopThread();
  // No response can arrive after this point. Fulfilling what is left prevents
  // Callers from waiting on a promise that would never be completed.
  failAllPending();
  client_.reset();
}

void RpcClient::connect() {
  if (loop_->isInLoopThread()) {
    // The connection callback runs on this thread, so waiting here would make
    // the handshake completion unreachable.
    assert(false && "RpcClient::connect() must not run on the loop thread");
    return;
  }
  if (connected_.load()) {
    return;
  }
  if (everConnected_.load()) {
    // TcpClient ignores connect() after a successful attempt, so waiting for
    // the promise below would never return.
    assert(false &&
           "RpcClient is single-session; create a new client to reconnect");
    return;
  }

  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A second concurrent connect() would wait on a promise that the first one
    // owns. One attempt at a time is the documented contract.
    assert(pendingConnect_ == nullptr && "connect() is already in progress");
    if (pendingConnect_ != nullptr) {
      return;
    }
    pendingConnect_ = promise;
  }

  client_->connect();
  // The loop resolves this on the connection callback or on a connect error.
  // There is no additional timeout here: the Connector already has one.
  future.wait();
}

void RpcClient::disconnect() { client_->disconnect(); }

Response RpcClient::Call(const Request &request) {
  return Call(request, 0.0);
}

Response RpcClient::Call(const Request &request, double timeoutSeconds) {
  if (loop_->isInLoopThread()) {
    // The response can only be delivered by this thread; blocking here is a
    // guaranteed deadlock. Fail fast instead.
    assert(false &&
           "RpcClient::Call() blocks; never call it on the loop thread");
    return badRequestResponse();
  }

  // The future is resolved by a matching response, the deadline, or a
  // connection-level failure. The caller must keep the client alive until
  // this returns.
  return CallAsync(request, timeoutSeconds).get();
}

void RpcClient::AsyncCall(const Request &request,
                          ResponseCallback callback) {
  AsyncCall(request, std::move(callback), 0.0);
}

void RpcClient::AsyncCall(const Request &request, ResponseCallback callback,
                          double timeoutSeconds) {
  assert(callback && "AsyncCall() requires a callback");
  if (!callback) {
    return;
  }

  // Posted from any thread; the loop owns the rest. startCallInLoop reports
  // every outcome through the completion on this same thread, so the guard
  // below is documentation as much as a check.
  loop_->runInLoop([this, request, timeoutSeconds,
                    callback = std::move(callback)]() mutable {
    loop_->assertInLoopThread();
    startCallInLoop(
        request,
        [this, callback = std::move(callback)](Response response) mutable {
          loop_->assertInLoopThread();
          callback(std::move(response));
        },
        timeoutSeconds);
  });
}

std::future<Response> RpcClient::CallAsync(const Request &request) {
  return CallAsync(request, 0.0);
}

std::future<Response>
RpcClient::CallAsync(const Request &request, double timeoutSeconds) {
  auto promise = std::make_shared<std::promise<Response>>();
  auto future = promise->get_future();
  AsyncCall(
      request,
      [promise](Response response) { promise->set_value(std::move(response)); },
      timeoutSeconds);
  return future;
}

void RpcClient::onConnection(const TransportPtr &transport) {
  loop_->assertInLoopThread();

  std::shared_ptr<std::promise<bool>> connectPromise;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connectPromise = std::move(pendingConnect_);
    pendingConnect_.reset();
  }

  if (transport->connected()) {
    connection_ = transport;
    connected_.store(true);
    everConnected_.store(true);
  } else {
    // The peer closed (or we disconnected): no response can arrive any more.
    connection_.reset();
    connected_.store(false);
    failAllPending();
  }

  // Fulfil the promise without holding mutex_: the woken thread immediately
  // tries to reacquire it in connect().
  if (connectPromise != nullptr) {
    connectPromise->set_value(transport->connected());
  }
}

void RpcClient::onConnectError(int /*savedErrno*/) {
  loop_->assertInLoopThread();

  std::shared_ptr<std::promise<bool>> connectPromise;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connectPromise = std::move(pendingConnect_);
    pendingConnect_.reset();
  }
  if (connectPromise != nullptr) {
    connectPromise->set_value(false);
  }
}

void RpcClient::onMessage(const TransportPtr & /*transport*/, Buffer *buf,
                          Timestamp /*receiveTime*/) {
  loop_->assertInLoopThread();

  for (;;) {
    MessageHeader header{};
    std::string body;
    const ParseResult result = frameReader_.parse(buf, &header, &body);
    if (result == ParseResult::kNeedMore) {
      return;
    }
    if (result == ParseResult::kProtocolError) {
      failConnection();
      return;
    }

    if (header.version != kVersion ||
        header.type != MessageType::kResponse) {
      failConnection();
      return;
    }

    Response response;
    response.status = header.status;
    if (!codec_.decodeResponse(body, &response)) {
      failConnection();
      return;
    }

    auto it = pendings_.find(header.requestId);
    if (it == pendings_.end()) {
      if (wasTimedOut(header.requestId)) {
        // The call already gave up. This response is late, not a violation.
        continue;
      }
      // An id we never sent means the two ends disagree about the stream;
      // silently ignoring it would only delay the failure.
      failConnection();
      return;
    }

    // Erase before invoking the completion: it may start another call, and the
    // table must not be mutated while this entry is being read.
    PendingCall pending = std::move(it->second);
    pendings_.erase(it);
    if (pending.timer != TimerId{}) {
      loop_->cancelTimer(pending.timer);
    }
    pending.complete(std::move(response));
  }
}

void RpcClient::startCallInLoop(const Request &request, Completion complete,
                                double timeoutSeconds) {
  loop_->assertInLoopThread();
  assert(complete && "a pending call needs a completion");

  if (connection_ == nullptr || !connected_.load()) {
    // No peer, no stream: the request was never sent. This also covers a
    // failed connect(): connect() does not throw, and the next call reports
    // the connection state here.
    complete(connectionClosedResponse());
    return;
  }

  std::string body;
  if (!codec_.encodeRequest(request, &body)) {
    // The request itself is invalid (empty service/method, body too large).
    // The connection is untouched.
    complete(badRequestResponse());
    return;
  }

  const uint32_t requestId = allocateRequestId();
  // Record the call before sending: the response cannot be read until this
  // callback returns to the loop, but keeping insertion first makes that
  // ordering explicit rather than incidental.
  auto inserted = pendings_.emplace(
      requestId, PendingCall{requestId, std::move(complete), TimerId{}});
  if (timeoutSeconds > 0.0) {
    inserted.first->second.timer = loop_->runAfter(
        timeoutSeconds, [this, requestId] { onTimeout(requestId); });
  }

  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kRequest;
  header.flags = 0;
  header.status = StatusCode::kOk;
  header.requestId = requestId;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encodedHeader{};
  encodeHeader(header, encodedHeader.data());

  std::string frame;
  frame.reserve(encodedHeader.size() + body.size());
  frame.assign(encodedHeader.data(), encodedHeader.size());
  frame += body;
  connection_->send(frame);
}

uint32_t RpcClient::allocateRequestId() {
  loop_->assertInLoopThread();

  // Skip 0 (reserved as invalid), anything still in flight, and any id kept
  // for late-response recognition. A collision can only happen after a full
  // 32-bit wrap, but choosing again is trivial and keeps the invariant
  // unconditional.
  for (;;) {
    const uint32_t id = nextRequestId_.fetch_add(1);
    if (id != 0 && pendings_.find(id) == pendings_.end() &&
        timedOutIdSet_.find(id) == timedOutIdSet_.end()) {
      return id;
    }
  }
}

void RpcClient::onTimeout(uint32_t id) {
  loop_->assertInLoopThread();

  auto it = pendings_.find(id);
  if (it == pendings_.end()) {
    return; // answered, failed, or already timed out
  }

  PendingCall pending = std::move(it->second);
  pendings_.erase(it);
  // The timer has already fired; no cancel is needed. Remember the id so a
  // response that is still on the wire is discarded instead of closing the
  // connection.
  rememberTimedOut(id);
  pending.complete(timeoutResponse());
}

void RpcClient::rememberTimedOut(uint32_t id) {
  loop_->assertInLoopThread();

  if (timedOutIdSet_.size() >= kMaxRememberedTimeouts) {
    timedOutIdSet_.erase(timedOutIds_.front());
    timedOutIds_.pop_front();
  }
  timedOutIds_.push_back(id);
  timedOutIdSet_.insert(id);
}

bool RpcClient::wasTimedOut(uint32_t id) const {
  loop_->assertInLoopThread();
  return timedOutIdSet_.find(id) != timedOutIdSet_.end();
}

void RpcClient::failAllPending() {
  loop_->assertInLoopThread();

  // Move the table out first: completions may start new calls, and filling a
  // new pending while iterating would invalidate the iterator.
  auto pendings = std::move(pendings_);
  pendings_.clear();
  for (auto &entry : pendings) {
    if (entry.second.timer != TimerId{}) {
      loop_->cancelTimer(entry.second.timer);
    }
    // Whatever caused this (peer close, orderly disconnect, destruction), no
    // response can arrive any more. kConnectionClosed tells the caller the
    // failure is the transport, not the remote handler.
    entry.second.complete(connectionClosedResponse());
  }
}

void RpcClient::failConnection() {
  loop_->assertInLoopThread();
  failAllPending();
  if (connection_ != nullptr) {
    connection_->forceClose();
  }
}

} // namespace reactornet::rpc
