#include <gtest/gtest.h>

#include "Buffer.h"

// Smoke test only. Its job is to prove the wiring end to end: that the test
// target builds, links against reactornet, and can drive real library code.
// Per-module tests live in sibling files.
//
// Note this deliberately exercises Buffer rather than asserting something like
// EXPECT_EQ(1 + 1, 2) — a trivial assertion would still pass if the library
// failed to link, which is exactly the failure this test exists to catch.
TEST(SmokeTest, BuildsLinksAndDrivesLibraryCode) {
    Buffer buf;
    EXPECT_EQ(buf.readableBytes(), 0u);

    buf.append("hello", 5);
    EXPECT_EQ(buf.readableBytes(), 6u);

    EXPECT_EQ(buf.retrieveAllAsString(), "hello");
    EXPECT_EQ(buf.readableBytes(), 0u);
}
