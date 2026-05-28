#include "telemetry/telemetry_thread.h"

#include <raylib.h>

// Platform shim: POSIX sockets on Linux/macOS, Winsock2 on Windows. Kept
// behind these aliases so the run() body below stays mostly platform-
// agnostic. Type aliases:
//   socket_t   - socket handle  (int on POSIX, SOCKET on Windows)
//   ssize_io   - recv return    (ssize_t on POSIX, int on Windows)
//   socklen_io - bind addr len  (socklen_t on POSIX, int on Windows)
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
        p.fd      = s;
        p.events  = POLLRDNORM;
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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "telemetry/phantom_parse.h"

namespace zg::telemetry {

TelemetryThread::TelemetryThread(int port, PhantomBuffer& buffer)
    : port_(port), buffer_(buffer) {}

TelemetryThread::~TelemetryThread() { stop(); }

void TelemetryThread::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&TelemetryThread::run, this);
}

void TelemetryThread::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void TelemetryThread::run() {
#if defined(_WIN32)
    // Winsock requires per-process initialization; do it inside the thread
    // so a process that never starts() doesn't pay the cost. WSAStartup
    // refcounts internally so multiple start/stop cycles are safe.
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

    if (::bind(sock,
               reinterpret_cast<sockaddr*>(&addr),
               static_cast<socklen_io>(sizeof(addr))) < 0) {
        sock_close(sock);
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return;
    }
    sock_fd_ = static_cast<std::intptr_t>(sock);
    listening_.store(true);

    // 1 KiB is comfortably above any well-formed Phantom payload (id + xyz
    // + ~200-char label + dozens of connection ids ≈ a few hundred bytes).
    // On POSIX we read with MSG_TRUNC so the kernel reports the *true*
    // datagram size and we can drop oversized payloads instead of feeding
    // their truncated head to the JSON parser. Windows recv() can't do
    // that — it instead returns SOCKET_ERROR with WSAEMSGSIZE when the
    // datagram overflowed the buffer; we treat that the same way.
    constexpr std::size_t kMaxPayload = 1024;
    std::array<char, kMaxPayload> buf{};
    while (running_.load(std::memory_order_relaxed)) {
        // 100ms tick so the worker can notice stop() without buffering blocked.
        const int rc = sock_poll_in_100ms(sock);
        if (rc <= 0) continue;

#if defined(_WIN32)
        const ssize_io n = ::recv(sock, buf.data(), static_cast<int>(buf.size()), 0);
        if (n == SOCKET_ERROR) {
            if (::WSAGetLastError() == WSAEMSGSIZE) continue;  // oversized; drop
            continue;
        }
        if (n <= 0) continue;
#else
        const ssize_io n = ::recv(sock, buf.data(), buf.size(), MSG_TRUNC);
        if (n <= 0) continue;
        if (static_cast<std::size_t>(n) > buf.size()) continue;  // oversized; drop
#endif

        auto phantom = parse_phantom(std::string_view(buf.data(), static_cast<std::size_t>(n)));
        if (!phantom) continue;
        phantom->spawn_time = GetTime();
        buffer_.add(*phantom);
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
