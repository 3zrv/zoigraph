#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "telemetry/phantom_buffer.h"

namespace zg::telemetry {

// UDP listener bound to 127.0.0.1:<port>. Each received datagram is parsed
// as one JSON phantom payload; valid phantoms are stamped with the current
// raylib time and pushed into the shared PhantomBuffer.
//
// Loopback-only because the directive's threat model is air-gapped — any
// telemetry source must run on the same host.
class TelemetryThread {
public:
    TelemetryThread(int port, PhantomBuffer& buffer);
    ~TelemetryThread();

    TelemetryThread(const TelemetryThread&)            = delete;
    TelemetryThread& operator=(const TelemetryThread&) = delete;

    void start();
    void stop();

    // True if the socket bound successfully at startup. Listener silently
    // gives up if the port is in use; callers can check this for surfacing
    // in the inspector. Note the bind happens on the worker thread, so
    // immediately after start() this can read false for a beat.
    bool listening() const { return listening_.load(); }

    // The port this listener was constructed for (bound or not).
    int port() const { return port_; }

private:
    void run();

    int               port_;
    PhantomBuffer&    buffer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> listening_{false};
    std::thread       worker_;
    // Stored as intptr_t so the same field fits a POSIX int file descriptor
    // and a Windows SOCKET handle (which is uintptr_t under the hood). The
    // .cpp casts to the platform-specific type at the boundary. -1 is the
    // not-open sentinel for both.
    std::intptr_t     sock_fd_ = -1;
};

}  // namespace zg::telemetry
