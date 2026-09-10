#include "TcpServer.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Buffer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

// --- Echo Server ---
// Usage: ./reactor_demo [numThreads]
// Default: single-threaded echo server on port 8080

class EchoServer {
public:
    EchoServer(EventLoop* loop, const InetAddress& listenAddr)
        : server_(loop, listenAddr, "EchoServer") {
        server_.setConnectionCallback(
            [](const TcpConnectionPtr& conn) {
                std::cout << "[Echo] " << conn->peerAddress().toIpPort()
                          << " -> " << conn->localAddress().toIpPort()
                          << " is " << (conn->connected() ? "UP" : "DOWN")
                          << std::endl;
            });
        server_.setMessageCallback(
            [](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
                // Echo back whatever we received
                std::string msg = buf->retrieveAllAsString();
                conn->send(msg);
            });
    }

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void start() { server_.start(); }

private:
    TcpServer server_;
};

// --- HTTP Server ---
// Usage: ./reactor_demo http
// Serves static files from the www/ directory

class HttpServer {
public:
    HttpServer(EventLoop* loop, const InetAddress& listenAddr,
               const std::string& docRoot)
        : server_(loop, listenAddr, "HttpServer"),
          docRoot_(docRoot) {
        server_.setConnectionCallback(
            [](const TcpConnectionPtr& conn) {
                std::cout << "[HTTP] " << conn->peerAddress().toIpPort()
                          << " -> " << conn->localAddress().toIpPort()
                          << " is " << (conn->connected() ? "UP" : "DOWN")
                          << std::endl;
            });
        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
                onMessage(conn, buf);
            });
    }

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void start() { server_.start(); }

private:
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
        // HTTP/1.1 keep-alive: handle every complete request already buffered.
        while (true) {
            const char* dataStart = buf->peek();
            const char* dataEnd = dataStart + buf->readableBytes();

            HttpRequest req;
            if (!req.parseRequest(dataStart, dataEnd)) {
                // Incomplete request, wait for more data
                return;
            }

            // Consume one complete request: header ends at \r\n\r\n (GET has no body)
            const char* headerEnd = std::search(dataStart, dataEnd, "\r\n\r\n", "\r\n\r\n" + 4);
            if (headerEnd == dataEnd) {
                return;
            }
            buf->retrieve((headerEnd + 4) - dataStart);

            HttpResponse response(false);  // keep-alive: do not close

            // Secure path: prevent directory traversal
            std::string filePath = docRoot_ + req.path();

            // Read file
            std::ifstream file(filePath, std::ios::binary);
            if (file.is_open()) {
                std::ostringstream oss;
                oss << file.rdbuf();
                std::string content = oss.str();

                response.setStatusCode(HttpResponse::k200Ok);
                response.setContentType(getMimeType(req.path()));
                response.setBody(content);
            } else {
                // 404 Not Found
                std::string notFoundBody = "<html><head><title>404 Not Found</title></head>"
                                           "<body><h1>404 Not Found</h1><p>"
                                           "The requested URL was not found on this server."
                                           "</p></body></html>";
                response.setStatusCode(HttpResponse::k404NotFound);
                response.setContentType("text/html");
                response.setBody(notFoundBody);
            }

            // Serialize response to a Buffer and send (connection stays open)
            Buffer responseBuf;
            response.appendToBuffer(&responseBuf);
            conn->send(responseBuf.retrieveAllAsString());
        }
    }

    static std::string getMimeType(const std::string& path) {
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".css")  return "text/css";
        if (path.size() >= 3 && path.substr(path.size() - 3) == ".js")   return "application/javascript";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".png")  return "image/png";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg")  return "image/jpeg";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg")  return "image/svg+xml";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".ico")  return "image/x-icon";
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") return "application/json";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".txt")  return "text/plain";
        return "application/octet-stream";
    }

    TcpServer server_;
    std::string docRoot_;
};

void printUsage(const char* prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " [numThreads]     Echo server (default: single-thread)\n"
              << "  " << prog << " http             HTTP static file server\n"
              << "  " << prog << " http [numThreads] HTTP server with worker threads\n"
              << "\nExamples:\n"
              << "  " << prog << "              Single-threaded echo on port 8080\n"
              << "  " << prog << " 4            Multi-threaded echo (4 workers)\n"
              << "  " << prog << " http         Single-threaded HTTP on port 8080\n"
              << "  " << prog << " http 4       Multi-threaded HTTP (4 workers)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== ReactorNet - C++ Network Library Demo ===\n" << std::endl;

    bool httpMode = false;
    int numThreads = 0;

    if (argc > 1) {
        if (std::string(argv[1]) == "http") {
            httpMode = true;
            if (argc > 2) {
                numThreads = std::stoi(argv[2]);
            }
        } else if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            numThreads = std::stoi(argv[1]);
        }
    }

    EventLoop loop;
    InetAddress listenAddr(8080);

    if (httpMode) {
        // Try common docRoot locations
        std::string docRoot = "../www/";
        {
            std::ifstream test(docRoot + "index.html");
            if (!test.is_open()) {
                docRoot = "www/";  // Fallback to project root
            }
        }
        std::cout << "Starting HTTP server on port 8080, docRoot=" << docRoot
                  << ", threads=" << numThreads << std::endl;
        HttpServer server(&loop, listenAddr, docRoot);
        server.setThreadNum(numThreads);
        server.start();
        std::cout << "Server listening on " << listenAddr.toIpPort() << std::endl;
        loop.loop();
    } else {
        std::cout << "Starting Echo server on port 8080, threads=" << numThreads << std::endl;
        EchoServer server(&loop, listenAddr);
        server.setThreadNum(numThreads);
        server.start();
        std::cout << "Server listening on " << listenAddr.toIpPort() << std::endl;
        loop.loop();
    }

    return 0;
}
