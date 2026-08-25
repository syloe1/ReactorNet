#include "InetAddress.h"
#include <cstring>
#include <iostream>
/*
struct sockaddr_in {
  sa_family_t sin_family;  // 协议族， AF_INET IPv4
  in_port_t sin_port;      // 网络大端序， 用htons/ntohs转换
  struct in_addr sin_addr; // 32位Ipv4二进制地址
  char sin_zero[8];        // 填充占位
}
struct in_addr
  {
    in_addr_t s_addr;
  };
*/
InetAddress::InetAddress(uint16_t port, const std::string &ip,
                         bool loopbackOnly) {
  std::memset(&addr_, 0, sizeof(addr_));
  addr_.sin_family = AF_INET; // IPV4
  if (loopbackOnly) {
    // htonl host to network long ,把本地序32->网络大端序， 存s_addr
    addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (ip.empty() || ip == "INADDR_ANY") {
    addr_.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    // inet_pton字符串IP->二进制网络序IP
    if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
      std::cerr << "[InetAddress] Invalid IP address: " << ip
                << ", falling back to INADDR_ANY" << std::endl;
      addr_.sin_addr.s_addr = htonl(INADDR_ANY);
    }
  }
  addr_.sin_port = htons(port);
}

InetAddress::InetAddress(const sockaddr_in &addr) : addr_(addr) {}
std::string InetAddress::toIpPort() const {
  char buf[64];
  // 先把IP转字符串存入buf
  ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
  char result[128];
  // snprintf 拼接 IP:端口，ntohs把网络序端口转回主机序数字
  std::snprintf(result, sizeof(result), "%s:%u", buf, ntohs(addr_.sin_port));
  return result;
}

//:: 代表全局命名空间，
// inet_ntop 二进制网络ip->可读字符串IP
// inet_pton 字符串IP -> 二进制网络IP
/*

struct sockaddr_in {
  sa_family_t sin_family;  // 协议族， AF_INET IPv4
  in_port_t sin_port;      // 网络大端序， 用htons/ntohs转换
  struct in_addr sin_addr; // 32位Ipv4二进制地址
  char sin_zero[8];        // 填充占位
}
*/
std::string InetAddress::toIp() const {
  char buf[64];
  ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
  return buf;
}