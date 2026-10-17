# RPC tests link the RPC library (which propagates the core headers and flags)
# and GoogleTest, which stays a dev-only dependency.
add_executable(reactornet_rpc_tests
    protocol_test.cpp
    frame_reader_test.cpp
    codec_test.cpp
)

target_link_libraries(reactornet_rpc_tests PRIVATE
    reactornet_rpc
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(reactornet_rpc_tests PROPERTIES TIMEOUT 60)
