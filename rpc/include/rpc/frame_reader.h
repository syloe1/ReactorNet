#pragma once

#include "rpc/protocol.h"

#include <cstddef>
#include <string>

namespace reactornet {
class Buffer;
}

namespace reactornet::rpc {

enum class ParseResult {
  kOk,           // a complete frame was consumed
  kNeedMore,     // the bytes so far are a valid prefix, nothing consumed
  kProtocolError // the stream cannot be a valid frame, nothing consumed
};

// Splits a TCP byte stream into RFC-0001 frames.
//
// parse() is stateless and re-entrant: callers may keep feeding the same
// Buffer from reads and call it again whenever bytes arrive. The rule is
// "consume only when complete":
//   - kNeedMore:     the buffer is untouched and must be kept for the next
//                    read. This includes a header split across reads and a
//                    body that has not fully arrived.
//   - kProtocolError: the buffer is untouched and *must not* be parsed again
//                    on this connection: v0.1 has no resynchronisation, so a
//                    bad magic or an oversized declared body is terminal. The
//                    caller closes the connection (possibly after sending an
//                    error response, #31/#40).
//   - kOk:           exactly one frame (header + body) is consumed.
//
// The declared bodyLen is checked against the size limit *before* waiting for
// the body or allocating anything, so a 4 GiB length prefix costs nothing
// (RFC-0001 §4.4). The default limit is 4 MiB of total frame size, header
// included; the protocol's absolute 16 MiB body ceiling always applies too,
// even if a caller raises the limit.
class FrameReader {
public:
  static constexpr size_t kDefaultMaxFrameSize = 4u * 1024u * 1024u;

  explicit FrameReader(size_t maxFrameSize = kDefaultMaxFrameSize);

  // Returns kOk and fills *header/*body when a whole frame is available;
  // kNeedMore while the frame is incomplete; kProtocolError if the leading
  // bytes can never form a valid frame. On kOk the frame is removed from
  // `buf` and `body` is replaced; on the other results both outputs and the
  // buffer are left untouched.
  ParseResult parse(Buffer *buf, MessageHeader *header, std::string *body);

  // Total frame size (header + body). Values below kHeaderSize are clamped to
  // kHeaderSize, the smallest frame that exists. Takes effect on the next
  // parse() and never raises the RFC's kMaxBodyLen ceiling.
  void setMaxFrameSize(size_t maxFrameSize);
  size_t maxFrameSize() const { return maxFrameSize_; }

private:
  size_t maxFrameSize_;
};

} // namespace reactornet::rpc
