#pragma once

#include "InetAddress.h"
#include "Transport.h"
#include "noncopyable.h"
#include "rpc/codec.h"
#include "rpc/frame_reader.h"
#include "rpc/service_registry.h"

#include <cstddef>
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
//
// Hardening (#40): two independent limits keep a hostile peer from turning a
// length prefix into memory pressure.
//   1. FrameReader rejects a declared bodyLen above the frame-size limit as
//      soon as the header arrives, before any body is buffered or allocated.
//   2. The message callback checks the bytes left in the connection's input
//      buffer. Parsing happens *after* the bytes have already entered the
//      Buffer, so limit (1) alone would let a peer grow the buffer with a
//      legal header followed by an endless body. The input-buffer check is the
//      second layer; FrameReader cannot see that memory.
// Framing failures are terminal because v0.1 has no resynchronisation. Bad
// magic closes silently (the peer may not be speaking RN at all); a decoded
// header that fails a policy check (version, type, flags, declared length)
// gets a status response and an orderly close, as RFC-0001 §4.6 requires.
class RpcServer : noncopyable {
public:
  RpcServer(EventLoop *loop, const InetAddress &listenAddr);
  ~RpcServer();

  // Valid until start(); registration afterwards is rejected by the registry.
  ServiceRegistry *registry() { return &registry_; }

  void setThreadNum(int numThreads);

  // Total frame size (header + body) accepted from a peer. Values below
  // kHeaderSize are clamped; the RFC's 16 MiB body ceiling still applies.
  // Takes effect on the next message.
  void setMaxFrameSize(size_t maxFrameSize);
  size_t maxFrameSize() const;

  // Maximum bytes allowed to remain unparsed in one connection's input buffer.
  // 0 (the default) means "follow maxFrameSize()": a partial frame can never
  // legitimately exceed one frame, so no separate number is needed unless the
  // caller wants a tighter bound. A smaller explicit value closes the
  // connection once a peer has sent more than that without completing a
  // frame. This is the second of the two layers described above.
  void setMaxInputBufferSize(size_t maxInputBufferSize);

  void start();

  // Delegates to TcpServer::listenAddress(): base loop thread, after start().
  InetAddress listenAddress() const;

private:
  void onMessage(const TransportPtr &conn, Buffer *buf, Timestamp receiveTime);
  void handleRequest(const TransportPtr &conn, const MessageHeader &header,
                     std::string_view body);
  void sendResponse(const TransportPtr &conn, uint32_t requestId,
                    const Response &response);

  // Sends `status` (when the header decoded far enough to have a request id)
  // and closes the stream in an orderly way. Used for policy rejections.
  void rejectAndClose(const TransportPtr &conn, uint32_t requestId,
                      StatusCode status);
  void logProtocolError(const TransportPtr &conn, const char *reason);

  std::unique_ptr<TcpServer> server_;
  ServiceRegistry registry_;
  FrameReader frameReader_;
  RawCodec codec_;
  size_t maxInputBufferSize_ = 0; // 0 = follow frameReader_.maxFrameSize()
};

} // namespace reactornet::rpc
