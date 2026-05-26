#pragma once

#include <mutex>
#include <vector>

#include "telemetry/phantom.h"

namespace zg::telemetry {

// Concurrent producer (telemetry thread) / consumer (render thread) buffer for
// Phantom nodes. Single mutex is adequate at the expected rate of incoming
// telemetry (handfuls per second, not thousands).
class PhantomBuffer {
public:
    void add(Phantom p);

    // Copies out every phantom whose spawn_time is within `ttl_seconds` of
    // `now`, dropping any that have expired from internal storage so memory
    // doesn't grow unbounded.
    void snapshot_and_expire(std::vector<Phantom>& out, float ttl_seconds, double now);

    std::size_t size() const;

private:
    mutable std::mutex   mu_;
    std::vector<Phantom> phantoms_;
};

}  // namespace zg::telemetry
