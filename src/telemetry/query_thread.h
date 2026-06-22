#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "telemetry/query_protocol.h"

namespace zg::telemetry {

// Opaque reply address: a sockaddr_in kept as raw bytes so this header stays
// socket-free. Carried verbatim from recvfrom to the matching sendto.
struct ReplyAddr {
    std::array<unsigned char, 28> bytes{};  // >= sizeof(sockaddr_in)
    int len = 0;
};

struct InboundQuery {
    QueryRequest request;
    ReplyAddr    reply_to;
};

// Loopback request/response channel on 127.0.0.1:<port> — sibling to the
// phantom-inject TelemetryThread, on a distinct port. The socket thread only
// moves bytes: recvfrom -> parse_query -> inbox; outbox -> sendto. The render
// thread owns the graph, drains the inbox once per frame, answers from its own
// state, and queues replies via send_reply. One frame of latency is nothing
// for an LLM round-trip, and the render thread never touches a socket.
class QueryThread {
public:
    explicit QueryThread(int port);
    ~QueryThread();
    QueryThread(const QueryThread&)            = delete;
    QueryThread& operator=(const QueryThread&) = delete;

    void start();
    void stop();

    // True once the socket bound successfully. The bind happens on the worker,
    // so this can read false for a beat right after start().
    bool listening() const { return listening_.load(); }
    int  port() const { return port_; }

    // Render-thread side. drain() moves out everything received since the last
    // call; send_reply() queues one datagram back to a request's origin.
    std::vector<InboundQuery> drain();
    void send_reply(const ReplyAddr& to, std::string datagram);

private:
    void run();

    struct Outbound { ReplyAddr to; std::string datagram; };

    int               port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> listening_{false};
    std::thread       worker_;
    std::intptr_t     sock_fd_ = -1;

    std::mutex                inbox_mu_;
    std::vector<InboundQuery> inbox_;
    std::mutex                outbox_mu_;
    std::vector<Outbound>     outbox_;
};

}  // namespace zg::telemetry
