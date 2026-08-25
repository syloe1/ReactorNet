#include "TcpServer.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include <cstring>
#include <iostream>
#include <cassert>

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                     const std::string& name)
    : baseLoop_(loop),
      name_(name),
      acceptor_(new Acceptor(loop, listenAddr)),
      started_(false),
      nextConnId_(1),
      threadNum_(0) {
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this,
                  std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {
    baseLoop_->assertInLoopThread();
    for (auto& conn : connections_) {
        TcpConnectionPtr connPtr(conn.second);
        conn.second.reset();
        connPtr->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, connPtr));
    }
}

void TcpServer::setThreadNum(int numThreads) {
    assert(!started_);
    threadNum_ = numThreads;
}

void TcpServer::start() {
    if (!started_) {
        started_ = true;

        // Create sub-reactor threads if requested
        if (threadNum_ > 0) {
            threadPool_.reserve(threadNum_);
            for (int i = 0; i < threadNum_; ++i) {
                auto thread = std::make_unique<EventLoopThread>();
                EventLoop* subLoop = thread->startLoop();
                if (threadInitCallback_) {
                    threadInitCallback_(subLoop);
                }
                subLoops_.push_back(subLoop);
                threadPool_.push_back(std::move(thread));
            }
        }
    }

    // Start accepting on the base loop
    if (!acceptor_->listening()) {
        baseLoop_->runInLoop(
            std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

std::vector<EventLoop*> TcpServer::getAllLoops() {
    std::vector<EventLoop*> result;
    result.push_back(baseLoop_);
    result.insert(result.end(), subLoops_.begin(), subLoops_.end());
    return result;
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    baseLoop_->assertInLoopThread();

    // Round-robin selection of an event loop
    EventLoop* ioLoop = baseLoop_;
    if (!subLoops_.empty()) {
        ioLoop = subLoops_[nextConnId_ % subLoops_.size()];
        ++nextConnId_;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "-%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
    std::string connName = name_ + buf;

    // Get local address from the socket
    sockaddr_in localAddr;
    socklen_t addrLen = sizeof(localAddr);
    std::memset(&localAddr, 0, sizeof(localAddr));
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) < 0) {
        std::cerr << "[TcpServer] getsockname failed: " << strerror(errno) << std::endl;
    }
    InetAddress localInetAddr(localAddr);

    TcpConnectionPtr conn(new TcpConnection(ioLoop, connName, sockfd,
                                            localInetAddr, peerAddr));

    connections_[connName] = conn;

    // Set up callbacks on the connection
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    // Establish the connection in its own IO loop
    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    baseLoop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    baseLoop_->assertInLoopThread();
    size_t n = connections_.erase(conn->name());
    (void)n;
    assert(n == 1);

    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));
}
