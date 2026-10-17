#include <gtest/gtest.h>

#include "rpc/service_registry.h"

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace reactornet::rpc;

// Tests for ServiceRegistry (#30): type erasure at registration time, exact
// misses, duplicate rejection, and the freeze-then-read-only contract that
// makes lock-free concurrent dispatch safe.
namespace {

ErasedMethod echo() {
  return [](std::string_view payload) { return std::string(payload); };
}

Request request(std::string service, std::string method,
                std::string payload = {}) {
  return Request{std::move(service), std::move(method), std::move(payload)};
}

} // namespace

TEST(ServiceRegistryTest, DispatchInvokesTheRegisteredHandler) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod("Echo", "Echo", echo()));
  EXPECT_EQ(registry.size(), 1u);

  Request req = request("Echo", "Echo", "hello bytes");
  Response resp;
  ASSERT_TRUE(registry.dispatch(req, &resp));
  EXPECT_EQ(resp.status, StatusCode::kOk);
  EXPECT_EQ(resp.payload, "hello bytes");
}

TEST(ServiceRegistryTest, HandlerReturnValueBecomesTheResponsePayload) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod(
      "Upper", "Upper", [](std::string_view payload) {
        std::string out(payload);
        for (char &c : out) {
          if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
          }
        }
        return out;
      }));

  Response resp;
  resp.status = StatusCode::kInternalError; // must be overwritten on success
  resp.payload = "stale";
  ASSERT_TRUE(registry.dispatch(request("Upper", "Upper", "mixed Case"), &resp));
  EXPECT_EQ(resp.status, StatusCode::kOk);
  EXPECT_EQ(resp.payload, "MIXED CASE");
}

TEST(ServiceRegistryTest, UnknownServiceAndUnknownMethodAreDistinguished) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod("Svc", "Known", echo()));

  Response resp;
  EXPECT_FALSE(registry.dispatch(request("Other", "Known"), &resp));
  EXPECT_EQ(resp.status, StatusCode::kUnknownService);

  EXPECT_FALSE(registry.dispatch(request("Svc", "Nope"), &resp));
  EXPECT_EQ(resp.status, StatusCode::kUnknownMethod);

  // A service exists with other methods, but not this one.
  EXPECT_FALSE(registry.dispatch(request("Svc", ""), &resp));
  EXPECT_EQ(resp.status, StatusCode::kUnknownMethod);
}

TEST(ServiceRegistryTest, MissClearsAStalePayload) {
  ServiceRegistry registry;
  Response resp;
  resp.payload = "stale";

  EXPECT_FALSE(registry.dispatch(request("Absent", "Method"), &resp));
  EXPECT_EQ(resp.status, StatusCode::kUnknownService);
  EXPECT_EQ(resp.payload, "");
}

TEST(ServiceRegistryTest, DuplicateRegistrationIsRejectedAndDoesNotReplace) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod(
      "Svc", "M", [](std::string_view) { return std::string("first"); }));

  EXPECT_FALSE(registry.registerMethod(
      "Svc", "M", [](std::string_view) { return std::string("second"); }));
  EXPECT_EQ(registry.size(), 1u);

  Response resp;
  ASSERT_TRUE(registry.dispatch(request("Svc", "M"), &resp));
  EXPECT_EQ(resp.payload, "first");
}

TEST(ServiceRegistryTest, SameMethodNameUnderDifferentServicesIsIndependent) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod(
      "A", "M", [](std::string_view) { return std::string("A.M"); }));
  ASSERT_TRUE(registry.registerMethod(
      "B", "M", [](std::string_view) { return std::string("B.M"); }));
  EXPECT_EQ(registry.size(), 2u);

  Response resp;
  ASSERT_TRUE(registry.dispatch(request("A", "M"), &resp));
  EXPECT_EQ(resp.payload, "A.M");
  ASSERT_TRUE(registry.dispatch(request("B", "M"), &resp));
  EXPECT_EQ(resp.payload, "B.M");
}

TEST(ServiceRegistryTest, EmptyNamesAndEmptyHandlersAreRejected) {
  ServiceRegistry registry;
  EXPECT_FALSE(registry.registerMethod("", "M", echo()));
  EXPECT_FALSE(registry.registerMethod("S", "", echo()));
  EXPECT_FALSE(registry.registerMethod("S", "M", ErasedMethod{}));
  EXPECT_EQ(registry.size(), 0u);
}

TEST(ServiceRegistryTest, FreezeBlocksFurtherRegistration) {
  ServiceRegistry registry;
  EXPECT_FALSE(registry.frozen());
  ASSERT_TRUE(registry.registerMethod("Svc", "M", echo()));

  registry.freeze();
  EXPECT_TRUE(registry.frozen());

  EXPECT_FALSE(registry.registerMethod("Svc", "Other", echo()));
  EXPECT_EQ(registry.size(), 1u);

  // The existing table keeps working, and freezing twice is harmless.
  Response resp;
  ASSERT_TRUE(registry.dispatch(request("Svc", "M", "still here"), &resp));
  EXPECT_EQ(resp.payload, "still here");
  registry.freeze();
  EXPECT_TRUE(registry.frozen());
}

// The registry is built before the server starts and only read afterwards;
// this case is the executable form of that claim. Run under TSan, it fails if
// dispatch ever mutates shared state.
TEST(ServiceRegistryTest, ConcurrentDispatchIsRaceFree) {
  ServiceRegistry registry;
  ASSERT_TRUE(registry.registerMethod("Echo", "Echo", echo()));
  registry.freeze();

  constexpr int kThreads = 8;
  constexpr int kIterations = 1000;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry, &failures, t] {
      for (int i = 0; i < kIterations; ++i) {
        const std::string payload =
            "payload-" + std::to_string(t) + "-" + std::to_string(i);
        Response resp;
        if (!registry.dispatch(request("Echo", "Echo", payload), &resp) ||
            resp.status != StatusCode::kOk || resp.payload != payload) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}
