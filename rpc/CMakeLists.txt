# RPC library, layered on top of the core. See docs/rfc/0002-rpc-layering.md:
# the dependency direction is one-way, and both halves of that rule are
# checked here/CI rather than left to discipline.

# Configure-time guard for the other direction: the core target must not link
# the RPC library. (The include-direction guard lives in ci.yml.)
get_target_property(_core_links reactornet LINK_LIBRARIES)
if(_core_links MATCHES "reactornet_rpc")
    message(FATAL_ERROR "reactornet (core) must not link reactornet_rpc")
endif()

add_library(reactornet_rpc STATIC
    src/protocol.cpp
    src/frame_reader.cpp
    src/raw_codec.cpp
    src/raw_serializer.cpp
    src/json_writer.cpp
    src/json_parser.cpp
    src/json_serializer.cpp
    src/service_registry.cpp
    src/rpc_server.cpp
    src/rpc_client.cpp
)

# PUBLIC so a consumer of the RPC library gets both the rpc/ and core headers,
# plus the core's pthread and warning flags.
target_include_directories(reactornet_rpc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(reactornet_rpc PUBLIC reactornet)
