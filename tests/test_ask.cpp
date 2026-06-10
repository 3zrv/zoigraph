#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/ask.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    static void sock_close(socket_t s) { ::closesocket(s); }
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr socket_t kInvalidSocket = -1;
    static void sock_close(socket_t s) { ::close(s); }
#endif

#include <chrono>
#include <vector>

using zg::app::last_nonblank_line;
using zg::app::tcp_probe_localhost;

namespace {

// Minimal loopback listener for exercising the probe's true path and its
// timeout path. Binds port 0 so the OS picks a free port.
struct Listener {
    socket_t fd   = kInvalidSocket;
    int      port = 0;

    explicit Listener(int backlog) {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd != kInvalidSocket);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = 0;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) == 0);
        REQUIRE(::listen(fd, backlog) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr),
                              &len) == 0);
        port = ntohs(addr.sin_port);
    }
    ~Listener() {
        if (fd != kInvalidSocket) sock_close(fd);
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

// Fire-and-forget non-blocking connect; returns the socket so the caller
// can keep the connection (and its accept-queue slot) alive.
socket_t filler_connect(int port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return s;
#if defined(_WIN32)
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
#else
    ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return s;
}

}  // namespace

TEST_CASE("last_nonblank_line: empty input returns empty") {
    CHECK(last_nonblank_line("").empty());
}

TEST_CASE("last_nonblank_line: single line without newline returns itself") {
    CHECK(last_nonblank_line("hello") == "hello");
}

TEST_CASE("last_nonblank_line: trailing newline is stripped") {
    CHECK(last_nonblank_line("hello\n") == "hello");
}

TEST_CASE("last_nonblank_line: trailing CRLF is stripped") {
    CHECK(last_nonblank_line("hello\r\n") == "hello");
}

TEST_CASE("last_nonblank_line: multiple lines returns the last non-blank one") {
    CHECK(last_nonblank_line("first\nsecond\nthird") == "third");
}

TEST_CASE("last_nonblank_line: trailing blank lines are skipped") {
    CHECK(last_nonblank_line("real\n\n\n") == "real");
}

TEST_CASE("last_nonblank_line: only-blank input returns empty") {
    CHECK(last_nonblank_line("\n\n\n").empty());
    CHECK(last_nonblank_line("\r\n\r\n").empty());
}

TEST_CASE("last_nonblank_line: surrounded by noise still picks the last line") {
    const std::string blob =
        "=== raw response ===\n"
        "{junk}\n"
        "=== parse ===\n"
        "schema invalid -- not sending UDP packet\n";
    CHECK(last_nonblank_line(blob) == "schema invalid -- not sending UDP packet");
}

TEST_CASE("tcp_probe_localhost: refused port returns false") {
    // Port 1 is reserved and never listened on by user processes; connect
    // returns ECONNREFUSED instantly. This is the "Ollama not running"
    // path the inspector relies on for fast error feedback.
    CHECK_FALSE(tcp_probe_localhost(1));
}

TEST_CASE("tcp_probe_localhost: another almost-certainly-closed port returns false") {
    // Belt-and-braces: a high ephemeral-range port nobody binds by default.
    // If this ever returns true on a CI box, we've got bigger problems
    // than this test.
    CHECK_FALSE(tcp_probe_localhost(57321));
}

TEST_CASE("tcp_probe_localhost: a live listener returns true") {
    Listener l(4);
    CHECK(tcp_probe_localhost(l.port));
}

TEST_CASE("tcp_probe_localhost: full accept backlog can't hang the probe") {
    // The reason the probe is non-blocking + poll at all. Fill a tiny
    // accept queue that nobody drains; further SYNs are silently dropped
    // (Linux/macOS), which parks a plain blocking connect in ~2 minutes
    // of kernel retransmits. The bounded probe must give up at its
    // timeout instead. Windows RSTs a full backlog, so the probe returns
    // fast there too -- the elapsed bound holds on all three OSes; only
    // the dropped-SYN path is Linux/macOS-specific.
    Listener l(1);
    std::vector<socket_t> fillers;
    for (int i = 0; i < 8; ++i) fillers.push_back(filler_connect(l.port));

    const auto t0 = std::chrono::steady_clock::now();
    const bool reachable = tcp_probe_localhost(l.port, 100);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    for (socket_t s : fillers) {
        if (s != kInvalidSocket) sock_close(s);
    }

    // Generous bound: orders of magnitude under the blocking-connect hang,
    // forgiving of CI scheduler jitter on top of the 100 ms budget.
    CHECK(elapsed_ms < 5000);
    // Whether the queue-overflow SYN got dropped (timeout -> false) or the
    // kernel still found room (true) varies by OS and backlog fudge
    // factors; the contract under test is the time bound, not the verdict.
    (void)reachable;
}
