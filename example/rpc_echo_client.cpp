#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "rpc/rpc_client.h"

#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <string>

using namespace reactornet;
using namespace reactornet::rpc;

// --- RPC Echo Client ---
// Usage: ./rpc_echo_client [port] [payload]   (defaults: 9000 "hello rpc")
// Start ./rpc_echo_server first. Call() blocks, so it runs on this thread
// while the EventLoopThread delivers the response.
int main(int argc, char *argv[]) {
  uint16_t port = 9000;
  std::string payload = "hello rpc";
  if (argc > 1) {
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
      std::cout << "Usage: " << argv[0] << " [port] [payload]\n";
      return 0;
    }
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    payload = argv[2];
  }

  EventLoopThread loopThread;
  EventLoop *loop = loopThread.startLoop();

  // shared_ptr, not unique_ptr: EventLoop::Functor is a std::function and
  // therefore requires a copy-constructible capture for the teardown task.
  auto client = std::make_shared<RpcClient>(loop,
                                            InetAddress(port, "127.0.0.1"));
  client->connect(); // returns once the handshake completes or fails

  const Response response = client->Call(Request{"Echo", "Echo", payload});
  std::cout << "status=" << static_cast<int>(response.status)
            << " payload=\"" << response.payload << "\"" << std::endl;

  // TcpClient is loop-thread owned, so destroy the client there and wait for
  // the task before the EventLoopThread goes away.
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  loop->runInLoop([client = std::move(client), done]() mutable {
    client.reset();
    done->set_value();
  });
  finished.wait();
  return 0;
}
