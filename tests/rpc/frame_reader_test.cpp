#include <gtest/gtest.h>

#include "Buffer.h"
#include "rpc/frame_reader.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

using namespace reactornet;
using namespace reactornet::rpc;

// Tests for the RFC-0001 frame splitter (#28).
//
// These cases never touch a socket: they feed bytes into a Buffer exactly as a
// read handler would, which is the point of keeping FrameReader independent of
// TcpConnection.
// ASan installs its own operator new/delete interceptors; replacing them in
// the test binary makes its alloc/dealloc bookkeeping see mismatched pairs in
// unrelated library code and abort. The allocation counter is therefore
// compiled in only for non-ASan builds. The behavioural half of the oversized
// case (kProtocolError with only the header present) runs under every build.
#if defined(__SANITIZE_ADDRESS__)
#define REACTORNET_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define REACTORNET_ASAN 1
#endif
#endif
#ifndef REACTORNET_ASAN
#define REACTORNET_ASAN 0
#endif

namespace {

#if !REACTORNET_ASAN
// Count allocations above 1 MiB for the "oversized length must not allocate"
// case. The counter is only inspected around a single parse() call, and no
// other test depends on allocator state.
std::atomic<size_t> &largeAllocations() {
  static std::atomic<size_t> count{0};
  return count;
}

constexpr size_t kLargeAllocationBytes = 1024u * 1024u;
#endif

std::string makeFrame(MessageType type, uint32_t requestId,
                      const std::string &body) {
  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = type;
  header.flags = 0;
  header.status = StatusCode::kOk;
  header.requestId = requestId;
  header.bodyLen = static_cast<uint32_t>(body.size());

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  return std::string(encoded.data(), encoded.size()) + body;
}

// A header that claims `declaredBodyLen` bytes, encoded on its own.
std::string makeBareHeader(uint32_t requestId, uint32_t declaredBodyLen) {
  MessageHeader header{};
  header.magic = kMagic;
  header.version = kVersion;
  header.type = MessageType::kRequest;
  header.requestId = requestId;
  header.bodyLen = declaredBodyLen;

  std::array<char, kHeaderSize> encoded{};
  encodeHeader(header, encoded.data());
  return std::string(encoded.data(), encoded.size());
}

} // namespace

#if !REACTORNET_ASAN
void *operator new(std::size_t size) {
  if (size > kLargeAllocationBytes) {
    largeAllocations().fetch_add(1, std::memory_order_relaxed);
  }
  if (void *p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif // !REACTORNET_ASAN

TEST(FrameReaderTest, ByteAtATimeYieldsExactlyOneFrame) {
  FrameReader reader;
  const std::string frame = makeFrame(MessageType::kRequest, 42, "hello");
  Buffer buf;

  MessageHeader header{};
  std::string body;
  for (size_t i = 0; i < frame.size(); ++i) {
    buf.append(frame.data() + i, 1);

    header.requestId = 0xA5A5A5A5u; // sentinel: only kOk may overwrite it
    body = "untouched";
    const ParseResult result = reader.parse(&buf, &header, &body);

    if (i + 1 < frame.size()) {
      EXPECT_EQ(result, ParseResult::kNeedMore)
          << "after " << (i + 1) << " byte(s)";
      EXPECT_EQ(header.requestId, 0xA5A5A5A5u);
      EXPECT_EQ(body, "untouched");
      EXPECT_EQ(buf.readableBytes(), i + 1) << "kNeedMore must not consume";
    } else {
      EXPECT_EQ(result, ParseResult::kOk);
      EXPECT_EQ(header.magic, kMagic);
      EXPECT_EQ(header.version, kVersion);
      EXPECT_EQ(header.type, MessageType::kRequest);
      EXPECT_EQ(header.requestId, 42u);
      EXPECT_EQ(header.bodyLen, 5u);
      EXPECT_EQ(body, "hello");
      EXPECT_EQ(buf.readableBytes(), 0u);
    }
  }
}

TEST(FrameReaderTest, TwoFramesInOneBufferAreParsedConsecutively) {
  FrameReader reader;
  const std::string first = makeFrame(MessageType::kRequest, 1, "first");
  const std::string second = makeFrame(MessageType::kResponse, 2, "second");

  Buffer buf;
  buf.append(first);
  buf.append(second);

  MessageHeader header{};
  std::string body;

  ASSERT_EQ(reader.parse(&buf, &header, &body), ParseResult::kOk);
  EXPECT_EQ(header.requestId, 1u);
  EXPECT_EQ(body, "first");
  EXPECT_EQ(buf.readableBytes(), second.size());

  ASSERT_EQ(reader.parse(&buf, &header, &body), ParseResult::kOk);
  EXPECT_EQ(header.requestId, 2u);
  EXPECT_EQ(body, "second");
  EXPECT_EQ(buf.readableBytes(), 0u);

  EXPECT_EQ(reader.parse(&buf, &header, &body), ParseResult::kNeedMore);
}

TEST(FrameReaderTest, HeaderSplitAcrossReadsIsAccepted) {
  FrameReader reader;
  const std::string frame = makeFrame(MessageType::kRequest, 7, "body");
  Buffer buf;

  MessageHeader header{};
  std::string body;

  buf.append(frame.data(), 8); // half a header
  EXPECT_EQ(reader.parse(&buf, &header, &body), ParseResult::kNeedMore);
  EXPECT_EQ(buf.readableBytes(), 8u);

  buf.append(frame.data() + 8, 8 + 3); // rest of header + 3 body bytes
  EXPECT_EQ(reader.parse(&buf, &header, &body), ParseResult::kNeedMore);
  EXPECT_EQ(buf.readableBytes(), 19u);

  buf.append(frame.data() + 19, frame.size() - 19);
  ASSERT_EQ(reader.parse(&buf, &header, &body), ParseResult::kOk);
  EXPECT_EQ(header.requestId, 7u);
  EXPECT_EQ(body, "body");
  EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(FrameReaderTest, IncompleteBodyKeepsWaitingWithoutConsuming) {
  FrameReader reader;
  const std::string header = makeBareHeader(9, 10);
  Buffer buf;
  buf.append(header);
  buf.append("abcd"); // 4 of the declared 10 bytes

  MessageHeader out{};
  std::string body;

  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(reader.parse(&buf, &out, &body), ParseResult::kNeedMore);
    EXPECT_EQ(buf.readableBytes(), kHeaderSize + 4 + i);
    EXPECT_EQ(body, "");
    buf.append("x", 1);
  }

  ASSERT_EQ(reader.parse(&buf, &out, &body), ParseResult::kOk);
  EXPECT_EQ(out.requestId, 9u);
  EXPECT_EQ(body, "abcdxxxxxx");
  EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(FrameReaderTest, OversizedDeclaredBodyFailsFastWithoutAllocating) {
  FrameReader reader; // default limit: 4 MiB total
  Buffer buf;
  buf.append(makeBareHeader(3, 64u * 1024u * 1024u));

  MessageHeader out{};
  out.requestId = 0xA5A5A5A5u;
  std::string body = "untouched";

#if !REACTORNET_ASAN
  const size_t allocationsBefore = largeAllocations().load();
#endif
  EXPECT_EQ(reader.parse(&buf, &out, &body), ParseResult::kProtocolError);
#if !REACTORNET_ASAN
  EXPECT_EQ(largeAllocations().load(), allocationsBefore)
      << "the declared bodyLen must be rejected before any big allocation";
#endif

  EXPECT_EQ(out.requestId, 0xA5A5A5A5u);
  EXPECT_EQ(body, "untouched");
  EXPECT_EQ(buf.readableBytes(), kHeaderSize) << "error must not consume";
}

TEST(FrameReaderTest, ConfiguredMaxFrameSizeIsAHardBoundary) {
  FrameReader reader(kHeaderSize + 4);

  Buffer exact;
  exact.append(makeFrame(MessageType::kRequest, 1, "abcd"));
  MessageHeader header{};
  std::string body;
  EXPECT_EQ(reader.parse(&exact, &header, &body), ParseResult::kOk);
  EXPECT_EQ(body, "abcd");

  Buffer oneOver;
  oneOver.append(makeFrame(MessageType::kRequest, 2, "abcde"));
  EXPECT_EQ(reader.parse(&oneOver, &header, &body), ParseResult::kProtocolError);

  // A limit smaller than the header is clamped: the empty-body frame is the
  // smallest frame that exists and must still be representable.
  reader.setMaxFrameSize(1);
  EXPECT_EQ(reader.maxFrameSize(), kHeaderSize);
  Buffer empty;
  empty.append(makeFrame(MessageType::kRequest, 3, ""));
  EXPECT_EQ(reader.parse(&empty, &header, &body), ParseResult::kOk);
  EXPECT_EQ(body, "");
}

TEST(FrameReaderTest, RfcBodyCeilingAppliesEvenWhenTheLimitIsRaised) {
  FrameReader reader;
  reader.setMaxFrameSize(64u * 1024u * 1024u);

  Buffer buf;
  buf.append(makeBareHeader(4, kMaxBodyLen + 1)); // 16 MiB + 1 > RFC ceiling

  MessageHeader header{};
  std::string body;
  EXPECT_EQ(reader.parse(&buf, &header, &body), ParseResult::kProtocolError);
  EXPECT_EQ(buf.readableBytes(), kHeaderSize);
}

TEST(FrameReaderTest, GarbageBeforeMagicIsRejectedWithoutResynchronising) {
  FrameReader reader;
  Buffer buf;
  buf.append("not a frame at all, definitely longer than 16 bytes");
  const size_t readable = buf.readableBytes();

  MessageHeader header{};
  std::string body;
  EXPECT_EQ(reader.parse(&buf, &header, &body), ParseResult::kProtocolError);
  EXPECT_EQ(buf.readableBytes(), readable);
}

TEST(FrameReaderTest, EmptyBodyFrameNeedsOnlyTheHeader) {
  FrameReader reader;
  Buffer buf;
  buf.append(makeFrame(MessageType::kResponse, 11, ""));

  MessageHeader header{};
  std::string body = "stale";
  ASSERT_EQ(reader.parse(&buf, &header, &body), ParseResult::kOk);
  EXPECT_EQ(header.type, MessageType::kResponse);
  EXPECT_EQ(header.requestId, 11u);
  EXPECT_EQ(header.bodyLen, 0u);
  EXPECT_EQ(body, "");
  EXPECT_EQ(buf.readableBytes(), 0u);
}
