#include "rpc_fixture.h"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace reactornet::rpc::testing {

namespace {
using namespace std::chrono_literals;
} // namespace

bool waitFor(const std::function<bool()> &pred, double timeoutSeconds) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeoutSeconds));
  for (;;) {
    if (pred()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    // Fine-grained polling keeps the helper generic; it is not a test-side
    // "sleep and hope", the predicate still decides when to stop.
    std::this_thread::sleep_for(1ms);
  }
}

RpcServerFixture::RpcServerFixture(Setup setup) {
  loop_ = thread_.startLoop();
  if (loop_ == nullptr) {
    error_ = "EventLoopThread did not start";
    return;
  }

  auto done = std::make_shared<std::promise<void>>();
  auto ready = done->get_future();
  loop_->runInLoop([this, setup = std::move(setup), done] {
    server_ = std::make_unique<RpcServer>(loop_, InetAddress(0, "127.0.0.1"));
    if (setup) {
      setup(server_->registry());
    } else {
      server_->registry()->registerMethod(
          "Echo", "Echo",
          [](std::string_view payload) { return std::string(payload); });
    }
    server_->start();
    port_ = server_->listenAddress().toPort();
    started_ = port_ != 0;
    done->set_value();
  });

  if (ready.wait_for(std::chrono::duration<double>(kWaitSeconds)) !=
      std::future_status::ready) {
    error_ = "the loop did not run the server setup task in time";
    return;
  }
  if (!started_) {
    error_ = "listenAddress() did not report a bound port";
  }
}

RpcServerFixture::~RpcServerFixture() {
  if (server_ == nullptr || loop_ == nullptr) {
    return;
  }

  // TcpServer must be destroyed on its loop. If the wait times out we still
  // leave server_ alone rather than destroying loop-owned state from here.
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  loop_->runInLoop([this, done] {
    server_.reset();
    started_ = false;
    done->set_value();
  });
  finished.wait_for(std::chrono::duration<double>(kWaitSeconds));

  // ~EventLoopThread() quits the loop and joins the thread.
}

} // namespace reactornet::rpc::testing
