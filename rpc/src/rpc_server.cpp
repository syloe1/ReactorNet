#include "rpc/rpc_server.h"

#include "EventLoop.h"
#include "TcpServer.h"

#include <array>
#include <functional>
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
      return;
    }
    if (result == ParseResult::kProtocolError) {
      // v0.1 has no resynchronisation: the stream position is lost, so the
      // only safe move is to drop the connection. #40 adds the error response
      // and the input-buffer growth limit.
      conn->forceClose();
      return;
    }

    if (header.version != kVersion ||
        header.type != MessageType::kRequest) {
      // A v0.1 server only serves REQUEST frames of version 1. Refusing here
      // keeps an unexpected frame from being interpreted as a call. #40 owns
      // the UNSUPPORTED_VERSION / UNSUPPORTED_MSG_TYPE responses.
      conn->forceClose();
      return;
    }

    handleRequest(conn, header, body);
  }
}

void RpcServer::handleRequest(const TransportPtr &conn,
                              const MessageHeader &header,
                              std::string_view body) {
  Request request;
  if (!codec_.decodeRequest(body, &request)) {
    // Malformed envelope: same terminal treatment as a bad frame (#40).
    conn->forceClose();
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
