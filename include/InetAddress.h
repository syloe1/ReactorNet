#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
// 封装Linux IPV4底层结构体sockaddr_in
class InetAddress {
public:
  explicit InetAddress(uint16_t port = 0, const std::string &ip = "",
                       bool loopbackOnly = false);
  explicit InetAddress(const sockaddr_in &addr);
  // 返回通用只读指针
  const sockaddr *getSockAddr() const {
    return reinterpret_cast<const sockaddr *>(&addr_);
  }
  // 返回IPV4原生可写指针
  sockaddr_in *getSockAddrIn() { return &addr_; }
  const sockaddr_in *getSockAddrIn() const { return &addr_; }
  // bind connect accept需要传入地址长度参数，
  // 返回内部sockaddr_in addr_结构体字节大小
  socklen_t getSockLen() const { return static_cast<socklen_t>(sizeof(addr_)); }
  // 拼接IP:port字符串
  std::string toIpPort() const;
  // 把二进制IPV4->字符串
  std::string toIp() const;
  // 取出可读， 可打印的主机序端口号
  // 网络传输是大端序
  // ntohs network  to host short把网络端口转回本地CPU字节序
  uint16_t toPort() const { return ntohs(addr_.sin_port); }
  // 并不是钩子， 普通的成员赋值接口
  void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }

private:
  sockaddr_in addr_;
};
