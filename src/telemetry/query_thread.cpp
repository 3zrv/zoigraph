#include "telemetry/query_thread.h"

// Platform shim: POSIX sockets on Linux/macOS, Winsock2 on Windows — mirrors
// telemetry_thread.cpp, but this channel uses recvfrom/sendto so a reply can
// return to the sender's ephemeral port.
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t   = SOCKET;
    using ssize_io   = int;
    using socklen_io = int;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    static int sock_close(socket_t s) { return ::closesocket(s); }
    static int sock_poll_in_100ms(socket_t s) {
        WSAPOLLFD p{};
        p.fd     = s;
        p.events = POLLRDNORM;
        return ::WSAPoll(&p, 1, 100);
    }
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t   = int;
    using ssize_io   = ssize_t;
    using socklen_io = socklen_t;
    static constexpr socket_t kInvalidSocket = -1;
    static int sock_close(socket_t s) { return ::close(s); }
    static int sock_poll_in_100ms(socket_t s) {
        pollfd p{s, POLLIN, 0};
        return ::poll(&p, 1, 100);
    }
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace zg::telemetry {

QueryThread::QueryThread(int port) : port_(port) {}

QueryThread::~QueryThread() { stop(); }

void QueryThread::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&QueryThread::run, this);
}

void QueryThread::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

std::vector<InboundQuery> QueryThread::drain() {
    std::vector<InboundQuery> out;
    std::lock_guard<std::mutex> g(inbox_mu_);
    out.swap(inbox_);
    return out;
}

void QueryThread::send_reply(const ReplyAddr& to, std::string datagram) {
    std::lock_guard<std::mutex> g(outbox_mu_);
    outbox_.push_back(Outbound{to, std::move(datagram)});
}

void QueryThread::run() {
#if defined(_WIN32)
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif

    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) {
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr),
               static_cast<socklen_io>(sizeof(addr))) < 0) {
        sock_close(sock);
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return;
    }
    sock_fd_ = static_cast<std::intptr_t>(sock);
    listening_.store(true);

    // Bound the inbox so a stalled render thread can't let a flood grow without
    // limit; excess is dropped, the client just times out and retries.
    constexpr std::size_t kMaxInbox   = 256;
    constexpr std::size_t kMaxPayload = 1024;
    std::array<char, kMaxPayload> buf{};

    while (running_.load(std::memory_order_relaxed)) {
        // Flush any replies the render thread queued since the last tick.
        std::vector<Outbound> pending;
        {
            std::lock_guard<std::mutex> g(outbox_mu_);
            pending.swap(outbox_);
        }
        for (const Outbound& o : pending) {
            sockaddr_in dest{};
            std::memcpy(&dest, o.to.bytes.data(),
                        std::min(sizeof(dest), o.to.bytes.size()));
            ::sendto(sock, o.datagram.data(),
                     static_cast<int>(o.datagram.size()), 0,
                     reinterpret_cast<sockaddr*>(&dest),
                     static_cast<socklen_io>(o.to.len));
        }

        // 100ms tick so stop() is noticed without blocking on recv.
        if (sock_poll_in_100ms(sock) <= 0) continue;

        sockaddr_in src{};
        socklen_io  srclen = sizeof(src);
#if defined(_WIN32)
        const ssize_io n = ::recvfrom(sock, buf.data(),
            static_cast<int>(buf.size()), 0,
            reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n == SOCKET_ERROR) continue;  // incl. WSAEMSGSIZE (oversized) -> drop
        if (n <= 0) continue;
#else
        const ssize_io n = ::recvfrom(sock, buf.data(), buf.size(), MSG_TRUNC,
            reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n <= 0) continue;
        if (static_cast<std::size_t>(n) > buf.size()) continue;  // oversized -> drop
#endif

        InboundQuery iq;
        iq.request = parse_query(
            std::string_view(buf.data(), static_cast<std::size_t>(n)));
        std::memcpy(iq.reply_to.bytes.data(), &src,
                    std::min(sizeof(src), iq.reply_to.bytes.size()));
        iq.reply_to.len = static_cast<int>(srclen);

        std::lock_guard<std::mutex> g(inbox_mu_);
        if (inbox_.size() < kMaxInbox) inbox_.push_back(std::move(iq));
    }

    listening_.store(false);
    if (sock_fd_ >= 0) {
        sock_close(static_cast<socket_t>(sock_fd_));
        sock_fd_ = -1;
    }
#if defined(_WIN32)
    ::WSACleanup();
#endif
}

}  // namespace zg::telemetry
