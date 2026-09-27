#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "Buffer.h"

// Unit tests for Buffer — the TCP reassembly core.
//
// readFd / writeFd are the only places in the library that touch a kernel
// socket buffer, so the fixture hands them a real connected fd rather than a
// mock: socketpair(AF_UNIX, SOCK_STREAM) gives a bidirectional pair with
// stream semantics (short reads/writes, a real kernel buffer) and no
// filesystem or network dependency.
//
// makeSpace is the other thing worth pinning down. It chooses between "slide
// the readable bytes down over the consumed prefix" and "grow the vector", and
// the move branch is only reachable after a particular append/retrieve
// rhythm — the kind a hand-run curl session never produces. Getting readIndex_
// wrong in that branch corrupts data silently, so each branch has its own test.
namespace {

// Buffer declares kCheapPrepend / kInitialSize as `static const size_t` with an
// in-class initializer but no out-of-line definition. Reading them where the
// lvalue-to-rvalue conversion is applied immediately is fine, but any *odr-use*
// — binding one to a reference, which is exactly what EXPECT_EQ does — needs a
// definition and fails to link. Copying them into namespace-scope constants
// takes the value at compile time and keeps this PR out of Buffer.h.
constexpr size_t kCheapPrepend = Buffer::kCheapPrepend;
constexpr size_t kInitialSize = Buffer::kInitialSize;

class BufferTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0)
        << "socketpair: " << std::strerror(errno);
  }

  void TearDown() override {
    for (int fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  // A single ::write() to a stream socket may legally be short, even here.
  // Looping keeps the byte count exact so each test lands in the readFd
  // branch it means to exercise.
  void peerWrite(const std::string &data) {
    size_t written = 0;
    while (written < data.size()) {
      const ssize_t n =
          ::write(fds_[1], data.data() + written, data.size() - written);
      // ASSERT_GT(n,0)：判断 write 调用是否出错
      // n>0：成功写入 n 个字节
      // n == -1：系统调用出错，测试直接失败终止
      ASSERT_GT(n, 0) << "peer write: " << std::strerror(errno);
      written += static_cast<size_t>(n);
    }
  }

  std::string peerRead(size_t len) {
    std::string out(len, '\0');
    size_t got = 0;
    while (got < len) {
      const ssize_t n = ::read(fds_[1], &out[got], len - got);
      if (n <= 0) {
        // n == -1：读系统调用出错
        // n == 0：读到 EOF（连接关闭）
        ADD_FAILURE() << "peer read: " << std::strerror(errno);
        break;
      }
      got += static_cast<size_t>(n);
    }
    out.resize(got);
    return out;
  }

  // fds_[0] is the end Buffer reads from and writes to; fds_[1] is the peer.
  int fds_[2] = {-1, -1};
};

// The three regions always tile the whole allocation exactly. Asserting this
// alongside the individual getters catches an index update that "balances out"
// only by coincidence.
void expectRegionsTileBuffer(const Buffer &buf) {
  EXPECT_EQ(buf.prependableBytes() + buf.readableBytes() + buf.writableBytes(),
            kCheapPrepend + kInitialSize);
}

TEST_F(BufferTest, InitialStateIsEmptyWithCheapPrependReserved) {
  Buffer buf;

  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.writableBytes(), kInitialSize);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  expectRegionsTileBuffer(buf);
}

TEST_F(BufferTest, AppendKeepsIndicesConsistent) {
  Buffer buf;
  buf.append(std::string(100, 'x'));

  EXPECT_EQ(buf.readableBytes(), 100u);
  EXPECT_EQ(buf.writableBytes(), kInitialSize - 100);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  expectRegionsTileBuffer(buf);
}

TEST_F(BufferTest, RetrieveAdvancesReadIndexOnly) {
  Buffer buf;
  buf.append(std::string(50, 'a'));
  buf.retrieve(20);

  // Consuming 20 bytes grows the prependable region by 20 and shrinks the
  // readable one by 20. writableBytes() is unaffected by a retrieve.
  EXPECT_EQ(buf.readableBytes(), 30u);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend + 20);
  EXPECT_EQ(buf.writableBytes(), kInitialSize - 50);
  EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), std::string(30, 'a'));
  expectRegionsTileBuffer(buf);
}

TEST_F(BufferTest, RetrieveExactlyReadableBytesResetsToCheapPrepend) {
  Buffer buf;
  buf.append(std::string(50, 'a'));
  buf.retrieve(50); // len == readableBytes() takes the retrieveAll() path

  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  EXPECT_EQ(buf.writableBytes(), kInitialSize);
}

TEST_F(BufferTest, RetrievePastEndClearsEverything) {
  Buffer buf;
  buf.append(std::string(50, 'a'));
  buf.retrieve(51); // more than is readable: drop it all rather than underflow

  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  EXPECT_EQ(buf.writableBytes(), kInitialSize);
}

TEST_F(BufferTest, RetrieveAsStringConsumesExactlyThatManyBytes) {
  Buffer buf;
  buf.append("hello world", 11);

  EXPECT_EQ(buf.retrieveAsString(5), "hello");
  EXPECT_EQ(buf.readableBytes(), 6u);
  EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), " world");
  expectRegionsTileBuffer(buf);
}

TEST_F(BufferTest, RetrieveAsStringLongerThanReadableReturnsEverything) {
  Buffer buf;
  buf.append("hello", 5);

  EXPECT_EQ(buf.retrieveAsString(100), "hello");
  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
}

TEST_F(BufferTest, RetrieveAllAsStringEmptiesTheBuffer) {
  Buffer buf;
  buf.append("abc", 3);
  buf.append("def", 3);

  EXPECT_EQ(buf.retrieveAllAsString(), "abcdef");
  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.writableBytes(), kInitialSize);
}

TEST_F(BufferTest, PeekPointsAtFirstReadableByteAndFollowsRetrieve) {
  Buffer buf;
  buf.append("abcdef", 6);

  EXPECT_EQ(buf.peek()[0], 'a');
  EXPECT_EQ(buf.peek(), buf.data().data() + buf.prependableBytes());

  buf.retrieve(2);

  EXPECT_EQ(buf.peek()[0], 'c');
  EXPECT_EQ(buf.peek()[buf.readableBytes() - 1], 'f');
  EXPECT_EQ(buf.peek(), buf.data().data() + buf.prependableBytes());
}

// The move branch: readIndex_ has drifted far enough right that the consumed
// prefix can absorb the new bytes, so the vector must NOT grow — the readable
// data is slid down to kCheapPrepend instead.
TEST_F(BufferTest, MakeSpaceMovesDataWhenPrependablePlusWritableSuffices) {
  Buffer buf;
  buf.append(std::string(kInitialSize, 'a')); // fills the writable area
  ASSERT_EQ(buf.writableBytes(), 0u);

  buf.retrieve(900); // read the bulk back out; readIndex_ is now large
  ASSERT_EQ(buf.readableBytes(), 124u);
  ASSERT_EQ(buf.prependableBytes(), kCheapPrepend + 900);
  ASSERT_EQ(buf.writableBytes(), 0u);

  const std::string kept(124, 'a');  // must survive the move untouched
  const std::string fresh(500, 'b'); // 500 > writableBytes(), so makeSpace runs
  buf.append(fresh);

  // writable(0) + prependable(908) >= 500 + kCheapPrepend, so this is the move
  // branch: the allocation is reused, not resized.
  EXPECT_EQ(buf.data().size(), kCheapPrepend + kInitialSize);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  EXPECT_EQ(buf.readableBytes(), 124u + 500u);
  EXPECT_EQ(buf.writableBytes(), kInitialSize - 124u - 500u);
  expectRegionsTileBuffer(buf);
  // The original 124 bytes must still be the first thing readable.
  EXPECT_EQ(buf.retrieveAllAsString(), kept + fresh);
}

// The grow branch: prependable + writable cannot cover the request, so the
// vector has to be resized to writeIndex_ + len.
TEST_F(BufferTest, MakeSpaceGrowsWhenPrependablePlusWritableIsTooSmall) {
  Buffer buf;
  buf.append(std::string(kInitialSize, 'a'));
  buf.retrieve(100); // small prefix consumed: not enough to absorb a big append

  const std::string big(2000, 'b');
  buf.append(big); // writable(0) + prependable(108) < 2000 + kCheapPrepend

  EXPECT_EQ(buf.data().size(), kCheapPrepend + kInitialSize + 2000);
  EXPECT_EQ(buf.readableBytes(), 924u + 2000u);
  // Unlike the move branch, growing does not compact: the consumed 100-byte
  // prefix is left in place and simply appended after, which is what makes
  // this branch cheap in the case it is chosen for.
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend + 100);
  // Growing must not disturb the bytes already in the readable region.
  EXPECT_EQ(buf.retrieveAllAsString(), std::string(924, 'a') + big);
}

TEST_F(BufferTest, FindCRLFLocatesTheSeparator) {
  Buffer buf;
  buf.append("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);

  const char *crlf = buf.findCRLF();
  ASSERT_NE(crlf, nullptr);
  EXPECT_EQ(crlf - buf.peek(), 14); // "GET / HTTP/1.1" is 14 bytes
  EXPECT_EQ(std::string(crlf, 2), "\r\n");
}

TEST_F(BufferTest, FindCRLFReturnsNullWhenNoSeparator) {
  Buffer buf;
  buf.append("no line ending here", 19);
  EXPECT_EQ(buf.findCRLF(), nullptr);

  // A lone '\r' as the final readable byte is not a separator — the search
  // must not read past the end of the readable region looking for its '\n'.
  Buffer trailing;
  trailing.append("abc\r", 4);
  EXPECT_EQ(trailing.findCRLF(), nullptr);

  // Nothing readable at all.
  Buffer empty;
  EXPECT_EQ(empty.findCRLF(), nullptr);
}

TEST_F(BufferTest, FindCRLFFindsSeparatorAtEndOfReadableRegion) {
  Buffer buf;
  buf.append("abc\r\n",
             5); // the separator occupies the last two readable bytes

  const char *crlf = buf.findCRLF();
  ASSERT_NE(crlf, nullptr)
      << "a separator at the very end is still a separator";
  EXPECT_EQ(crlf - buf.peek(), 3);
}

TEST_F(BufferTest, ReadFdReadsIntoWritableSpace) {
  Buffer buf;

  const std::string payload = "ping from the peer";
  peerWrite(payload);

  int savedErrno = 0;
  const ssize_t n = buf.readFd(fds_[0], &savedErrno);

  EXPECT_EQ(n, static_cast<ssize_t>(payload.size()));
  EXPECT_EQ(savedErrno, 0);
  EXPECT_EQ(buf.readableBytes(), payload.size());
  EXPECT_EQ(buf.writableBytes(), kInitialSize - payload.size());
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  EXPECT_EQ(buf.retrieveAllAsString(), payload);
}

// readv fills vec[0] (the writable region) first and spills the remainder into
// the 64KB stack extrabuf, which readFd then appends. Pushing more than
// writableBytes() in one go is the only way to reach that branch.
TEST_F(BufferTest, ReadFdSpillsIntoExtraBufferWhenPayloadExceedsWritableSpace) {
  Buffer buf;
  ASSERT_EQ(buf.writableBytes(), kInitialSize);

  const std::string payload(2000,
                            'z'); // > kInitialSize, so it cannot fit whole
  peerWrite(payload);

  int savedErrno = 0;
  const ssize_t n = buf.readFd(fds_[0], &savedErrno);

  ASSERT_EQ(n, static_cast<ssize_t>(payload.size()));
  ASSERT_GT(static_cast<size_t>(n), kInitialSize)
      << "payload must exceed the writable region for this branch to run";
  EXPECT_EQ(savedErrno, 0);
  EXPECT_EQ(buf.readableBytes(), payload.size());
  // writeIndex_ = buffer_.size() then append(extrabuf, n - writable) resizes
  // to kCheapPrepend + writable + (n - writable) == kCheapPrepend + n.
  EXPECT_EQ(buf.data().size(), kCheapPrepend + payload.size());
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
  // The bytes written straight into the writable region and the ones that
  // came back through extrabuf must join up in the right order.
  EXPECT_EQ(buf.retrieveAllAsString(), payload);
}

TEST_F(BufferTest, WriteFdSendsReadableBytesAndConsumesThem) {
  Buffer buf;

  const std::string payload(300, 'w');
  buf.append(payload);
  ASSERT_EQ(buf.readableBytes(), payload.size());

  int savedErrno = 0;
  const ssize_t n = buf.writeFd(fds_[0], &savedErrno);

  EXPECT_EQ(n, static_cast<ssize_t>(payload.size()));
  EXPECT_EQ(savedErrno, 0);
  EXPECT_EQ(peerRead(payload.size()), payload);
  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.prependableBytes(), kCheapPrepend);
}

} // namespace
