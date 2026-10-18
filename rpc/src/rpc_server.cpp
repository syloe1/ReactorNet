#include "rpc/rpc_server.h"

#include "EventLoop.h"
#include "TcpServer.h"

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <string>

namespace reactornet::rpc {

RpcServer::RpcServer(EventLoop *loop, const InetAddress &listenAddr)
    : server_(std::make_unique<TcpServer>(loop, listenAddr, "RpcServer")) {
  server_->setMessageCallback(std::bind(&RpcServer::onMessage, this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3));
}

RpcServer::~RpcServer() = default;

void RpcServer::setThreadNum(int numThreads) {
  server_->setThreadNum(numThreads);
}

void RpcServer::setMaxFrameSize(size_t maxFrameSize) {
  frameReader_.setMaxFrameSize(maxFrameSize);
}

size_t RpcServer::maxFrameSize() const { return frameReader_.maxFrameSize(); }

void RpcServer::setMaxInputBufferSize(size_t maxInputBufferSize) {
  // 0 keeps the "follow maxFrameSize()" default. Any explicit value below one
  // header would reject even a truncated header, so clamp it like the reader.
  maxInputBufferSize_ =
      maxInputBufferSize == 0 ? 0 : std::max(maxInputBufferSize, kHeaderSize);
}

void RpcServer::start() {
  // Freeze before accepting: from here on dispatch() runs concurrently on
  // every IO loop, and the registry's lock-free reads depend on there being
  // no writers after this point.
  registry_.freeze();
  server_->start();
}

InetAddress RpcServer::listenAddress() const {
  return server_->listenAddress();
}

void RpcServer::onMessage(const TransportPtr &conn, Buffer *buf,
                          Timestamp /*receiveTime*/) {
  // One read may contain several frames (or half of one). Keep consuming
  // complete frames; kNeedMore means the rest stays in the connection's input
  // buffer for the next read.
  for (;;) {
    MessageHeader header{};
    std::string body;
    const ParseResult result = frameReader_.parse(buf, &header, &body);
    if (result == ParseResult::kNeedMore) {
      // Second layer of the DoS defence (see rpc_server.h): the bytes are
      // already in the Buffer before parse() sees them, so a legal header
      // followed by an endless body can still grow the Buffer. Bound what is
      // left unparsed, not just what the header declares.
      const size_t limit = maxInputBufferSize_ != 0
                               ? maxInputBufferSize_
                               : frameReader_.maxFrameSize();
      if (buf->readableBytes() > limit) {
        logProtocolError(conn,
                         "input buffer exceeded the configured limit; "
                         "peer sent an incomplete or oversized frame");
        conn->forceClose();
      }
      return;
    }
    if (result == ParseResult::kProtocolError) {
      // v0.1 has no resynchronisation: the stream position is lost, so the
      // only safe move is to drop the connection. Two cases share this
      // result: undecodable framing (bad magic) and a declared bodyLen over
      // the limit. If the header itself decodes, the id is still usable for
      // an error response; otherwise the peer may not be speaking this
      // protocol at all and a response would be meaningless.
      MessageHeader decoded{};
      if (buf->readableBytes() >= kHeaderSize &&
          decodeHeader(buf->peek(), buf->readableBytes(), &decoded)) {
        logProtocolError(conn,
                         "declared body exceeds the frame-size limit");
        rejectAndClose(conn, decoded.requestId, StatusCode::kBodyTooLarge);
      } else {
        logProtocolError(conn, "bad magic; closing without a response");
        conn->forceClose();
      }
      return;
    }

    if (header.version != kVersion) {
      // A v0.1 server only serves version 1. Answer with the RFC status so a
      // newer client learns why, then close: the rest of the stream is
      // unknowable.
      logProtocolError(conn, "unsupported protocol version");
      rejectAndClose(conn, header.requestId, StatusCode::kUnsupportedVersion);
      return;
    }
    if (header.type != MessageType::kRequest) {
      logProtocolError(conn, "unexpected message type");
      rejectAndClose(conn, header.requestId, StatusCode::kUnsupportedMsgType);
      return;
    }
    if (header.flags != 0) {
      // RFC-0001 §4.5: v0.1 writes neither flag bit and rejects any set bit
      // (including unknown ones) rather than guessing at semantics.
      logProtocolError(conn, "unsupported frame flags");
      rejectAndClose(conn, header.requestId, StatusCode::kUnsupportedFlag);
      return;
    }

    handleRequest(conn, header, body);
  }
}

void RpcServer::rejectAndClose(const TransportPtr &conn, uint32_t requestId,
                               StatusCode status) {
  // send() before shutdown(): shutdown() flips the connection to
  // kDisconnecting, and send() is a no-op in that state. The transport
  // flushes the queued response before half-closing the write side.
  sendResponse(conn, requestId, Response{status, ""});
  conn->shutdown();
}

void RpcServer::logProtocolError(const TransportPtr &conn, const char *reason) {
  std::cerr << "[RpcServer] " << conn->name() << " ("
            << conn->peerAddress().toIpPort() << "): " << reason << std::endl;
}

void RpcServer::handleRequest(const TransportPtr &conn,
                              const MessageHeader &header,
                              std::string_view body) {
  Request request;
  if (!codec_.decodeRequest(body, &request)) {
    // The frame itself is intact but the envelope inside it is not (truncated
    // or zero-length names). The request id is known, so RFC-0001 §4.6 says
    // to answer MALFORMED_BODY and then close; the stream cannot continue.
    logProtocolError(conn, "malformed request envelope");
    rejectAndClose(conn, header.requestId, StatusCode::kMalformedBody);
    return;
  }

  Response response;
  registry_.dispatch(request, &response);
  sendResponse(conn, header.requestId, response);
}

void RpcServer::sendResponse(const TransportPtr &conn, uint32_t requestId,
                             const Response &response) {
  std::string body;
  if (!codec_.encodeResponse(response, &body)) {
    // The handler produced a payload the codec cannot frame. That is a local
    // failure; the peer gets no response and the connection is dropped.
    logProtocolError(conn, "response body exceeds the codec limit");
    conn->forceClose();
    return;
  }

  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kResponse;
  header.flags = 0;
  header.status = response.status;
  header.requestId = requestId;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encodedHeader{};
  encodeHeader(header, encodedHeader.data());

  // One send per frame keeps the header and body ordered in the output buffer
  // and gives the write path a single contiguous write.
  std::string frame(encodedHeader.data(), encodedHeader.size());
  frame += body;
  conn->send(frame);
}

} // namespace reactornet::rpc
