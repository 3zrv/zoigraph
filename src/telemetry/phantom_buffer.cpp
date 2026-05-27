#include "telemetry/phantom_buffer.h"

#include <algorithm>

namespace zg::telemetry {

void PhantomBuffer::add(Phantom p) {
    std::lock_guard<std::mutex> lock(mu_);
    phantoms_.push_back(std::move(p));
}

void PhantomBuffer::snapshot_and_expire(std::vector<Phantom>& out,
                                        float ttl_seconds,
                                        double now) {
    std::lock_guard<std::mutex> lock(mu_);
    phantoms_.erase(
        std::remove_if(phantoms_.begin(), phantoms_.end(),
                       [&](const Phantom& p) { return (now - p.spawn_time) > ttl_seconds; }),
        phantoms_.end());
    out = phantoms_;
}

void PhantomBuffer::remove(long long id) {
    std::lock_guard<std::mutex> lock(mu_);
    phantoms_.erase(
        std::remove_if(phantoms_.begin(), phantoms_.end(),
                       [id](const Phantom& p) { return p.id == id; }),
        phantoms_.end());
}

void PhantomBuffer::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    phantoms_.clear();
}

std::size_t PhantomBuffer::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return phantoms_.size();
}

}  // namespace zg::telemetry
