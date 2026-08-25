#include "Buffer.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

Buffer::Buffer(size_t initialSize)
    : buffer_(kCheapPrepend + initialSize), readIndex_(kCheapPrepend),
      writeIndex_(kCheapPrepend) {}

void Buffer::retrieve(size_t len) {
  if (len < readableBytes()) {
    readIndex_ += len;
  } else { // 要消费的长度 >= 全部可读， 直接清空所有数据
    retrieveAll();
  }
}

void Buffer::retrieveAll() {
  readIndex_ = kCheapPrepend;
  writeIndex_ = kCheapPrepend;
}

std::string Buffer::retrieveAsString(size_t len) {
  size_t actualLen = std::min(len, readableBytes());
  // 从可读起始指针拷贝actualLen到字符串
  std::string result(peek(), actualLen);
  retrieve(actualLen); // 消费后移动指针
  return result;
}

std::string Buffer::retrieveAllAsString() {
  return retrieveAsString(readableBytes());
}
// append() -> ensureWritableBytes(len) -> makeSpace(len)
void Buffer::append(const char *data, size_t len) {
  ensureWritableBytes(len);
  // 内存拷贝： 外部data复制到buffer可写区
  std::copy(data, data + len, begin() + writeIndex_);
  writeIndex_ += len;
}

void Buffer::append(const std::string &str) {
  append(str.data(), str.size());
}

void Buffer::ensureWritableBytes(size_t len) {
  if (writableBytes() < len) {
    makeSpace(len);
  }
}

// Read from fd using readv: first into writable buffer space,
// then into a stack buffer (64KB) if more data is available.
// This avoids premature buffer growth for large reads.
ssize_t Buffer::readFd(int fd, int *savedErrno) {
  char extrabuf[65536]; // 64KB【‘ “
  const size_t writable = writableBytes();
  // struct iovec
  //   {
  //     void *iov_base;	/* Pointer to data.  */
  //     size_t iov_len;	/* Length of data.  */
  //   };

  iovec vec[2];
  vec[0].iov_base = begin() + writeIndex_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof(extrabuf);

  const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
  ssize_t n = ::readv(fd, vec, iovcnt);
  if (n < 0) {
    *savedErrno = errno;
  } else if (static_cast<size_t>(n) <= writable) {
    // All data fit in the buffer
    writeIndex_ += n;
  } else {
    // Data overflowed into extrabuf, append to buffer
    writeIndex_ = buffer_.size();
    append(extrabuf, n - writable);
  }
  return n;
}

ssize_t Buffer::writeFd(int fd, int *savedErrno) {
  ssize_t n = ::write(fd, peek(), readableBytes());
  if (n < 0) {
    *savedErrno = errno;
  } else {
    retrieve(n);
  }
  return n;
}

const char *Buffer::findCRLF() const {
  const char *crlf =
      std::search(peek(), peek() + readableBytes(), "\r\n", "\r\n" + 2);
  return crlf == peek() + readableBytes() ? nullptr : crlf;
}

void Buffer::makeSpace(size_t len) {
  // If prependable space + writable space is enough, move data forward
  if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
    // Need to grow the buffer
    buffer_.resize(writeIndex_ + len);
  } else {
    // Move readable data to the beginning of the buffer
    size_t readable = readableBytes();
    std::copy(begin() + readIndex_, begin() + writeIndex_,
              begin() + kCheapPrepend);
    readIndex_ = kCheapPrepend;
    writeIndex_ = readIndex_ + readable;
  }
}
