cmake_minimum_required(VERSION 3.10)
project(ReactorNet VERSION 1.0.0 LANGUAGES CXX)

# C++17 required for std::make_unique, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Collect all source files
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
    src/main.cpp
)

# Create the executable
add_executable(reactor_demo ${SOURCES})

# Link libraries (Linux: pthread is often needed implicitly, but be explicit)
find_package(Threads REQUIRED)
target_link_libraries(reactor_demo PRIVATE Threads::Threads)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(reactor_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Install
install(TARGETS reactor_demo DESTINATION bin)
