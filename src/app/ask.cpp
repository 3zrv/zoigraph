#include "app/ask.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    static int sock_close(socket_t s) { return ::closesocket(s); }
    #define ZG_POPEN  _popen
    #define ZG_PCLOSE _pclose
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr socket_t kInvalidSocket = -1;
    static int sock_close(socket_t s) { return ::close(s); }
    #define ZG_POPEN  ::popen
    #define ZG_PCLOSE ::pclose
#endif

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <utility>

namespace zg::app {

namespace {

// Connect-completion logic after a non-blocking ::connect returned
// "in progress": wait for writability up to timeout_ms, then read
// SO_ERROR -- a writable socket signals completion, not success, and
// ECONNREFUSED is only visible in SO_ERROR.
bool await_connect(socket_t s, int timeout_ms) {
#if defined(_WIN32)
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(s, &wfds);
    FD_SET(s, &efds);  // winsock reports connect failure via exceptfds
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(0, nullptr, &wfds, &efds, &tv) <= 0) return false;
    if (FD_ISSET(s, &efds)) return false;
    int err = 0;
    int len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&err), &len) != 0) return false;
    return err == 0;
#else
    pollfd pfd{};
    pfd.fd     = s;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, timeout_ms) != 1) return false;  // timeout
    int       err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return false;
    return err == 0;
#endif
}

bool set_nonblocking(socket_t s) {
#if defined(_WIN32)
    u_long nb = 1;
    return ::ioctlsocket(s, FIONBIO, &nb) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool connect_in_progress() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

}  // namespace

bool tcp_probe_localhost(int port, int timeout_ms) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    bool ok = false;
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s != kInvalidSocket && set_nonblocking(s)) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<unsigned short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const int rc = ::connect(s,
            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            ok = true;  // completed synchronously (common on loopback)
        } else if (connect_in_progress()) {
            ok = await_connect(s, timeout_ms);
        }
    }
    if (s != kInvalidSocket) sock_close(s);
#if defined(_WIN32)
    WSACleanup();
#endif
    return ok;
}

std::string last_nonblank_line(const std::string& s) {
    if (s.empty()) return "";
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r')) --end;
    if (end == 0) return "";
    std::size_t start = end;
    while (start > 0 && s[start - 1] != '\n' && s[start - 1] != '\r') --start;
    return s.substr(start, end - start);
}

LlmAsk::~LlmAsk() {
    // Worker thread may still be inside popen() waiting on Ollama. Joining
    // serialises a clean shutdown but means the app waits for whatever
    // request is in flight. Acceptable for MVP -- model calls bound at
    // ~15s on this hardware. Future improvement: signal the subprocess.
    if (worker_.joinable()) worker_.join();
}

void LlmAsk::start(std::string db_path, long long anchor_id) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (state_ == State::Thinking) return;
        state_ = State::Thinking;
        msg_   = "thinking with ollama:llama3.2:3b...";
    }
    // Make sure the previous worker has finished before starting a new one.
    // start() is only called by the inspector when state != Thinking, so a
    // prior worker is either Done or Err-something; join is fast.
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, p = std::move(db_path), anchor_id]() {
        run(p, anchor_id);
    });
}

void LlmAsk::set_script_path(std::string path) {
    std::lock_guard<std::mutex> g(mu_);
    script_path_ = std::move(path);
}

LlmAsk::Snapshot LlmAsk::snapshot() const {
    std::lock_guard<std::mutex> g(mu_);
    return {state_, msg_};
}

void LlmAsk::run(std::string db_path, long long anchor_id) {
    std::string script;
    {
        std::lock_guard<std::mutex> g(mu_);
        script = script_path_;
    }

    // Fail clearly if the emitter script isn't where we resolved it to be
    // (a partial install, or a dist tarball missing scripts/) rather than
    // surfacing python's opaque "can't open file" through the subprocess.
    {
        std::error_code ec;
        if (!std::filesystem::exists(script, ec)) {
            std::lock_guard<std::mutex> g(mu_);
            state_ = State::ErrSubprocess;
            msg_   = "emitter script not found: " + script;
            return;
        }
    }

    if (!tcp_probe_localhost(11434)) {
        std::lock_guard<std::mutex> g(mu_);
        state_ = State::ErrNoOllama;
        msg_   = "Ollama not reachable at localhost:11434 -- "
                 "is `ollama serve` running?";
        return;
    }

    // Command assembly: project name validator restricts the path to
    // [A-Za-z0-9_-] so shell metacharacters can't appear; the script path is
    // resolved by main and quoted. Quoting is defence-in-depth, not load-bearing.
    std::ostringstream cmd;
    cmd << "python3 \"" << script << "\" emit "
        << "--backend ollama --model llama3.2:3b "
        << "--db \"" << db_path << "\" "
        << "--anchor-id " << anchor_id << " "
        << "2>&1";

    FILE* pipe = ZG_POPEN(cmd.str().c_str(), "r");
    if (!pipe) {
        std::lock_guard<std::mutex> g(mu_);
        state_ = State::ErrSubprocess;
        msg_   = "popen failed -- is python3 on PATH?";
        return;
    }
    std::string captured;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        captured += buf.data();
    }
    const int rc = ZG_PCLOSE(pipe);

    std::lock_guard<std::mutex> g(mu_);
    if (rc == 0) {
        state_ = State::Done;
        msg_   = "done -- phantom should appear shortly";
    } else {
        state_ = State::ErrSubprocess;
        const std::string tail = last_nonblank_line(captured);
        if (tail.empty()) {
            msg_ = "subprocess failed (exit " + std::to_string(rc) + ")";
        } else {
            msg_ = tail;
        }
    }
}

}  // namespace zg::app
