#include "EventLoop.h"
#include "InetAddress.h"
#include "rpc/rpc_server.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

using namespace reactornet;
using namespace reactornet::rpc;

// --- RPC Echo Server ---
// Usage: ./rpc_echo_server [port]   (default: 9000)
// Pair it with ./rpc_echo_client [port] [payload].
int main(int argc, char *argv[]) {
  uint16_t port = 9000;
  if (argc > 1) {
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
      std::cout << "Usage: " << argv[0] << " [port]\n"
                << "  port  listen port (default: 9000; 0 picks a free one)\n";
      return 0;
    }
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  EventLoop loop;
  RpcServer server(&loop, InetAddress(port, "0.0.0.0"));
  server.registry()->registerMethod(
      "Echo", "Echo",
      [](std::string_view payload) { return std::string(payload); });
  server.start();

  // listenAddress() is only valid on this (the base loop) thread, after
  // start(); with port 0 it reports the port the kernel picked.
  std::cout << "RPC echo server listening on "
            << server.listenAddress().toIpPort() << std::endl;
  loop.loop();
  return 0;
}
