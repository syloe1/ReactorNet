cmake_minimum_required(VERSION 3.10)
project(ReactorNet VERSION 1.0.0 LANGUAGES CXX)

# C++17 required for std::make_unique, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link libraries (Linux: pthread is often needed implicitly, but be explicit)
# NOTE: must come before target_link_libraries — the imported target has to exist first.
find_package(Threads REQUIRED)

# Collect library sources (everything that is not a main())
set(SOURCES
    src/InetAddress.cpp
    src/Socket.cpp
    src/Channel.cpp
    src/EPollPoller.cpp
    src/EventLoop.cpp
    src/Timer.cpp
    src/TimerQueue.cpp
    src/Buffer.cpp
    src/TcpConnection.cpp
    src/Acceptor.cpp
    src/TcpServer.cpp
    src/EventLoopThread.cpp
    src/HttpRequest.cpp
    src/HttpResponse.cpp
)

# Create the static library
add_library(reactornet STATIC ${SOURCES})

# PUBLIC: anything linking reactornet inherits the include path and pthread.
target_include_directories(reactornet PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(reactornet PUBLIC Threads::Threads)

# Compiler warnings (PUBLIC so consumers inherit the same flags)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(reactornet PUBLIC -Wall -Wextra -Wpedantic)
endif()

# Echo Server example
add_executable(echo_server example/echo_server.cpp)
target_link_libraries(echo_server PRIVATE reactornet)

# Http Server example
add_executable(http_server example/http_server.cpp)
target_link_libraries(http_server PRIVATE reactornet)
# Install
install(TARGETS echo_server http_server DESTINATION bin)

# --- Tests -----------------------------------------------------------------
# GoogleTest is a dev-only dependency: nothing under tests/ is ever linked into
# reactornet, so the library keeps its "no third-party dependencies" property.
# include(CTest) defines the standard BUILD_TESTING option (ON by default) and
# calls enable_testing(); with -DBUILD_TESTING=OFF tests/ is skipped entirely
# and GoogleTest is never downloaded.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

