#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

// Non-contiguous read/write buffer designed for TCP stream processing.
// Uses prependable + readable + writable layout:
//   [prependable (8 bytes)] [readable bytes] [writable bytes]
//
// readIndex_ points to start of readable data.
// writeIndex_ points to end of readable data (start of writable area).
// [可前置区 prependable][可读区 readable][可写区 writable] 0 readIndex_
//     writeIndex_ buffer_.size()
class Buffer {
  // TCP 缓冲区， 解决毡包，半包
public:
  static const size_t kCheapPrepend = 8;   // 前面预留8字节
  static const size_t kInitialSize = 1024; // 初始可写空间1024字节
  explicit Buffer(size_t initialSize = kInitialSize);
  size_t readableBytes() const { return writeIndex_ - readIndex_; }
  size_t writableBytes() const { return buffer_.size() - writeIndex_; }
  size_t prependableBytes() const { return readIndex_; }
  // 获取可读数据起始地址
  // begin是Buffer内存首地址
  const char *peek() const { return begin() + readIndex_; }
  char *peek() { return begin() + readIndex_; }
  // 消费len个可读字节，把readIndex_向后偏移len
  void retrieve(size_t len);
  void retrieveAll(); // 清空全部数据
  std::string retrieveAsString(size_t len);
  std::string retrieveAllAsString();

  // IO接口， 网络核心
  ssize_t readFd(int fd, int *savedErrno);  // 读取内核缓冲区数据，
                                            // 存入当前buffer
  ssize_t writeFd(int fd, int *savedErrno); // 把buffer可读数据写入socket fd

  // write写入接口
  void append(const char *data, size_t len);
  void append(const std::string &str);

  // 确保有空间
  void ensureWritableBytes(size_t len);

  // 在可读区间[peek(), peek() + readableBytes()] 查找换行符\r\n
  const char *findCRLF() const; // 找到返回\r的指针
  // 在Buffer当前未消费的数据， 查找\r\n换行， 切分一行完整报文
  const std::vector<char> &data() const {
    return buffer_; // 返回底层vector的const只读引用
  }

private:
  // 可读写指针
  char *begin() { return buffer_.data(); }
  // 只读指针
  const char *begin() const { return buffer_.data(); }
  void makeSpace(size_t len);
  std::vector<char> buffer_;
  size_t readIndex_;  // 标记可读数据的起始位置
  size_t writeIndex_; // 标记可写数据的起始位置
};
