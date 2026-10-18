#pragma once

#include "InetAddress.h"
#include "Transport.h"
#include "noncopyable.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"
#include "rpc/service_registry.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace reactornet {
class EventLoop;
class TcpServer;
} // namespace reactornet

namespace reactornet::rpc {

using TransportPtr = Transport::TransportPtr;

// Server side of the v0.1 RPC stack: TcpServer + FrameReader + Codec +
// ServiceRegistry, wired into one chain per connection.
//
// Threading: every connection's bytes, partial frame, dispatch and response are
// handled on that connection's IO loop, so handlers need no locks and the
// registry is frozen by start(). With setThreadNum(N), different connections
// live on different loops and may dispatch concurrently; FrameReader and
// RawCodec hold no per-message state, so sharing them is safe.
//
// A blocking handler stalls every connection on its loop. Giving handlers a
// separate thread pool is deliberately out of scope (RFC-0002).
//
// Lifetime: like TcpServer, construct/start/destroy on the base loop thread.
// All failures that leave the byte stream unusable (bad magic, wrong version or
// type, malformed envelope) close the connection; the error-response and
// input-buffer-limit policy is #40. Handler exceptions are not caught here:
// mapping them to INTERNAL_ERROR is #39.
class RpcServer : noncopyable {
public:
  RpcServer(EventLoop *loop, const InetAddress &listenAddr);
  ~RpcServer();

  // Valid until start(); registration afterwards is rejected by the registry.
  ServiceRegistry *registry() { return &registry_; }

  void setThreadNum(int numThreads);
  void start();

  // Delegates to TcpServer::listenAddress(): base loop thread, after start().
  InetAddress listenAddress() const;

private:
  void onMessage(const TransportPtr &conn, Buffer *buf, Timestamp receiveTime);
  void handleRequest(const TransportPtr &conn, const MessageHeader &header,
                     std::string_view body);
  void sendResponse(const TransportPtr &conn, uint32_t requestId,
                    const Response &response);

  std::unique_ptr<TcpServer> server_;
  ServiceRegistry registry_;
  FrameReader frameReader_;
  RawCodec codec_;
};

} // namespace reactornet::rpc
