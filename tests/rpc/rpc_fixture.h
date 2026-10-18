#pragma once

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "InetAddress.h"
#include "rpc/rpc_server.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace reactornet::rpc::testing {

// Shared timeout for tests that wait on an event instead of sleeping.
constexpr double kWaitSeconds = 5.0;

// Polls `pred` until it becomes true or the deadline passes. The polling
// interval is an implementation detail; tests never sleep for a fixed duration
// and hope that something happened.
bool waitFor(const std::function<bool()> &pred,
             double timeoutSeconds = kWaitSeconds);

// Starts an EventLoopThread plus an RpcServer bound to port 0 on that loop.
//
// The server is created, configured, started and queried entirely on the loop
// thread, which is where RpcServer requires it to live. The destructor reverses
// the process there too: destroying the server from the test thread would trip
// the loop-thread assertion in TcpServer.
//
// Default setup registers "Echo.Echo", which returns the payload unchanged.
// Pass a setup callback to register other methods; it runs before start(),
// while the registry is still mutable.
class RpcServerFixture {
public:
  using Setup = std::function<void(ServiceRegistry *)>;

  explicit RpcServerFixture(Setup setup = nullptr);
  ~RpcServerFixture();

  RpcServerFixture(const RpcServerFixture &) = delete;
  RpcServerFixture &operator=(const RpcServerFixture &) = delete;

  // False when the loop or the server failed to start; `error()` says why.
  bool started() const { return started_; }
  const std::string &error() const { return error_; }

  EventLoop *loop() const { return loop_; }
  InetAddress addr() const { return InetAddress(port_, "127.0.0.1"); }
  uint16_t port() const { return port_; }

  // Valid after construction; the registry is frozen by then.
  ServiceRegistry *registry() { return server_ ? server_->registry() : nullptr; }
  RpcServer *server() { return server_.get(); }

  // Runs `fn` on the fixture loop and waits for it. Returns false on timeout.
  template <typename Fn> bool runOnLoop(Fn &&fn) {
    if (loop_ == nullptr) {
      return false;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    loop_->runInLoop([fn = std::forward<Fn>(fn), done]() mutable {
      fn();
      done->set_value();
    });
    return finished.wait_for(std::chrono::duration<double>(kWaitSeconds)) ==
           std::future_status::ready;
  }

private:
  EventLoopThread thread_;
  EventLoop *loop_ = nullptr;
  std::unique_ptr<RpcServer> server_;
  uint16_t port_ = 0;
  bool started_ = false;
  std::string error_;
};

} // namespace reactornet::rpc::testing
