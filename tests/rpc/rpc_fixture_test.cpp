#include <gtest/gtest.h>

#include "rpc_fixture.h"

#include <dirent.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace reactornet::rpc;
using namespace reactornet::rpc::testing;
using namespace std::chrono_literals;

// Tests for the reusable server fixture itself (#33). The interesting path is
// teardown: a server that is not destroyed on its loop asserts, and a leak
// shows up as a growing fd count or as an ASan report.
namespace {

size_t openFileDescriptorCount() {
  DIR *dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return 0;
  }
  size_t count = 0;
  while (::readdir(dir) != nullptr) {
    ++count;
  }
  ::closedir(dir);
  return count;
}

TEST(RpcFixture, StartsOnAnEphemeralPortWithEchoRegistered) {
  RpcServerFixture fixture;
  ASSERT_TRUE(fixture.started()) << fixture.error();
  EXPECT_NE(fixture.port(), 0u);
  EXPECT_EQ(fixture.addr().toPort(), fixture.port());
  EXPECT_TRUE(fixture.registry()->frozen());
  EXPECT_EQ(fixture.registry()->size(), 1u);

  Response response;
  EXPECT_TRUE(fixture.registry()->dispatch(
      Request{"Echo", "Echo", "payload"}, &response));
  EXPECT_EQ(response.status, StatusCode::kOk);
  EXPECT_EQ(response.payload, "payload");
}

TEST(RpcFixture, RunOnLoopExecutesOnTheLoopThread) {
  RpcServerFixture fixture;
  ASSERT_TRUE(fixture.started()) << fixture.error();

  bool ran = false;
  bool inLoop = false;
  ASSERT_TRUE(fixture.runOnLoop([&] {
    ran = true;
    inLoop = fixture.loop()->isInLoopThread();
  }));
  EXPECT_TRUE(ran);
  EXPECT_TRUE(inLoop);
}

TEST(RpcFixture, SetupCallbackCanRegisterAnotherMethod) {
  RpcServerFixture fixture([](ServiceRegistry *registry) {
    registry->registerMethod("Custom", "Method",
                             [](std::string_view) { return std::string("ok"); });
  });
  ASSERT_TRUE(fixture.started()) << fixture.error();
  EXPECT_EQ(fixture.registry()->size(), 1u);

  Response response;
  EXPECT_TRUE(fixture.registry()->dispatch(
      Request{"Custom", "Method", ""}, &response));
  EXPECT_EQ(response.payload, "ok");

  EXPECT_FALSE(fixture.registry()->dispatch(
      Request{"Echo", "Echo", ""}, &response));
  EXPECT_EQ(response.status, StatusCode::kUnknownService);
}

TEST(RpcFixture, FiftyCreateDestroyCyclesLeakNoFileDescriptors) {
  // Warm up once so lazy allocations (epoll, eventfd, allocator arenas) do not
  // count as a leak.
  {
    RpcServerFixture warmup;
    ASSERT_TRUE(warmup.started()) << warmup.error();
  }

  const size_t before = openFileDescriptorCount();
  for (int i = 0; i < 50; ++i) {
    RpcServerFixture fixture;
    ASSERT_TRUE(fixture.started()) << "cycle " << i << ": " << fixture.error();
    ASSERT_NE(fixture.port(), 0u);
  }
  const size_t after = openFileDescriptorCount();

  EXPECT_EQ(after, before) << "the fixture leaked file descriptors";
}

TEST(WaitFor, ReturnsAsSoonAsThePredicateHolds) {
  // std::atomic: the predicate runs on this thread while the setter writes from
  // another. A plain bool here is itself a data race and makes TSan fail the
  // test before it can check waitFor().
  std::atomic<bool> ready{false};
  std::thread setter([&ready] {
    std::this_thread::sleep_for(20ms);
    ready.store(true);
  });
  const bool observed = waitFor([&ready] { return ready.load(); }, 2.0);
  setter.join();
  EXPECT_TRUE(observed);
}

TEST(WaitFor, ReturnsFalseAfterTheTimeout) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(waitFor([] { return false; }, 0.05));
  EXPECT_GE(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(50));
}

} // namespace
