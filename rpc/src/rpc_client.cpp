#include "rpc/rpc_client.h"

#include "EventLoop.h"
#include "TcpClient.h"

#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace reactornet::rpc {

namespace {

Response errorResponse() { return Response{StatusCode::kInternalError, ""}; }

} // namespace

RpcClient::RpcClient(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop), client_(std::make_unique<TcpClient>(loop, serverAddr,
                                                       "RpcClient")) {
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
  assert(!callInFlight_.load() &&
         "destroy RpcClient only after every Call() has returned");
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
  if (loop_->isInLoopThread()) {
    // The response can only be delivered by this thread; blocking here is a
    // guaranteed deadlock. Fail fast instead.
    assert(false &&
           "RpcClient::Call() blocks; never call it on the loop thread");
    return errorResponse();
  }

  bool expected = false;
  if (!callInFlight_.compare_exchange_strong(expected, true)) {
    assert(false && "v0.1 RpcClient allows one in-flight request");
    return errorResponse();
  }

  // RFC-0001 requires a non-zero request id. Skipping 0 on wrap keeps that
  // invariant for the whole 32-bit range.
  uint32_t requestId = 0;
  do {
    requestId = nextRequestId_.fetch_add(1);
  } while (requestId == 0);

  auto promise = std::make_shared<std::promise<Response>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCall_ = promise;
    pendingRequestId_ = requestId;
  }

  loop_->runInLoop([this, request, requestId] {
    sendRequestInLoop(request, requestId);
  });

  Response response =
      future.get(); // resolved by a response or a connection failure

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCall_.reset();
    pendingRequestId_ = 0;
  }
  callInFlight_.store(false);
  return response;
}

void RpcClient::onConnection(const TransportPtr &transport) {
  loop_->assertInLoopThread();

  std::shared_ptr<std::promise<bool>> connectPromise;
  std::shared_ptr<std::promise<Response>> callPromise;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connectPromise = std::move(pendingConnect_);
    pendingConnect_.reset();
    if (!transport->connected()) {
      callPromise = std::move(pendingCall_);
      pendingCall_.reset();
      pendingRequestId_ = 0;
    }
  }

  if (transport->connected()) {
    connection_ = transport;
    connected_.store(true);
    everConnected_.store(true);
  } else {
    // The peer closed (or we disconnected): a call still waiting for its
    // response can no longer be completed.
    connection_.reset();
    connected_.store(false);
  }

  // Fulfil the promises without holding mutex_: wakeups immediately try to
  // reacquire it in connect()/Call().
  if (connectPromise != nullptr) {
    connectPromise->set_value(transport->connected());
  }
  if (callPromise != nullptr) {
    callPromise->set_value(errorResponse());
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

    bool havePending = false;
    uint32_t expectedRequestId = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      havePending = pendingCall_ != nullptr;
      expectedRequestId = pendingRequestId_;
    }
    if (!havePending || header.requestId != expectedRequestId) {
      // An unsolicited or mismatched response means the peer and this client
      // disagree about the stream; there is no safe way to continue.
      failConnection();
      return;
    }

    resolveCall(response);
  }
}

void RpcClient::sendRequestInLoop(const Request &request, uint32_t requestId) {
  loop_->assertInLoopThread();

  if (connection_ == nullptr || !connected_.load()) {
    resolveCall(errorResponse());
    return;
  }

  std::string body;
  if (!codec_.encodeRequest(request, &body)) {
    resolveCall(errorResponse());
    return;
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

void RpcClient::resolveCall(const Response &response) {
  loop_->assertInLoopThread();

  std::shared_ptr<std::promise<Response>> promise;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    promise = std::move(pendingCall_);
    pendingCall_.reset();
    pendingRequestId_ = 0;
  }
  if (promise != nullptr) {
    promise->set_value(response);
  }
}

void RpcClient::failConnection() {
  loop_->assertInLoopThread();
  resolveCall(errorResponse());
  if (connection_ != nullptr) {
    connection_->forceClose();
  }
}

} // namespace reactornet::rpc
