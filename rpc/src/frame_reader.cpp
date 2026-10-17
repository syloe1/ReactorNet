#include "rpc/frame_reader.h"

#include "Buffer.h"

#include <algorithm>

namespace reactornet::rpc {

FrameReader::FrameReader(size_t maxFrameSize) { setMaxFrameSize(maxFrameSize); }

void FrameReader::setMaxFrameSize(size_t maxFrameSize) {
  maxFrameSize_ = std::max(maxFrameSize, kHeaderSize);
}

ParseResult FrameReader::parse(Buffer *buf, MessageHeader *header,
                               std::string *body) {
  if (buf == nullptr || header == nullptr || body == nullptr) {
    return ParseResult::kProtocolError;
  }

  // Peek only. A header split across reads is simply incomplete, not an error.
  const size_t readable = buf->readableBytes();
  if (readable < kHeaderSize) {
    return ParseResult::kNeedMore;
  }

  MessageHeader candidate{};
  if (!decodeHeader(buf->peek(), readable, &candidate)) {
    return ParseResult::kProtocolError;
  }

  // Bound the declared length before waiting for it and before allocating
  // anything for it. This is the classic length-prefix DoS: without this
  // check a 16-byte header could make the reader reserve gigabytes while the
  // peer sends nothing. The protocol ceiling applies even when the caller
  // configured a larger frame size.
  const size_t maxBodyForFrame = maxFrameSize_ - kHeaderSize;
  const size_t maxBody =
      std::min(maxBodyForFrame, static_cast<size_t>(kMaxBodyLen));
  if (candidate.bodyLen > maxBody) {
    return ParseResult::kProtocolError;
  }

  const size_t frameSize = kHeaderSize + candidate.bodyLen;
  if (readable < frameSize) {
    return ParseResult::kNeedMore;
  }

  // Complete frame: copy the body out, then consume. Doing it in this order
  // means a frame is never partially consumed, so the reader can stay
  // stateless and the caller never has to track a half-read body.
  body->assign(buf->peek() + kHeaderSize, candidate.bodyLen);
  buf->retrieve(frameSize);
  *header = candidate;
  return ParseResult::kOk;
}

} // namespace reactornet::rpc
