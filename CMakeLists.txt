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

# Create the demo executable: only main.cpp, everything else comes from the library
add_executable(reactor_demo src/main.cpp)
target_link_libraries(reactor_demo PRIVATE reactornet)

# Install
install(TARGETS reactor_demo DESTINATION bin)
