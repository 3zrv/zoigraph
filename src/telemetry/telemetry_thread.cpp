#include "telemetry/telemetry_thread.h"

#include <raylib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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
    sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) return;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
        return;
    }
    listening_.store(true);

    std::array<char, 4096> buf{};
    while (running_.load(std::memory_order_relaxed)) {
        pollfd p{sock_fd_, POLLIN, 0};
        // 100ms tick so the worker can notice stop() without buffering blocked.
        const int rc = ::poll(&p, 1, 100);
        if (rc <= 0) continue;
        if (!(p.revents & POLLIN)) continue;

        const ssize_t n = ::recv(sock_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) continue;

        auto phantom = parse_phantom(std::string_view(buf.data(), static_cast<std::size_t>(n)));
        if (!phantom) continue;
        phantom->spawn_time = GetTime();
        buffer_.add(*phantom);
    }

    listening_.store(false);
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

}  // namespace zg::telemetry
