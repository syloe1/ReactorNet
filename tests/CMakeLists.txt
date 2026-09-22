# GoogleTest is a *dev* dependency. Nothing here is ever linked into reactornet
# itself, so the library keeps its "no third-party dependencies" property.
#
# NOTE: we fetch the codeload tarball by URL rather than using GIT_REPOSITORY.
# github.com:443 is not reachable from every network (a git clone simply hangs),
# while codeload.github.com is. Pinning URL_HASH is also stricter than pinning a
# git tag: a tag can be moved on the remote, a content hash cannot.
include(FetchContent)

FetchContent_Declare(
    googletest
    URL      https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.14.0
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
)
FetchContent_MakeAvailable(googletest)

add_executable(reactornet_tests
    smoke_test.cpp
)

target_link_libraries(reactornet_tests PRIVATE
    reactornet
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(reactornet_tests)
