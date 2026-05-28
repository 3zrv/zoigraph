#include "app/phantom_lifecycle.h"

#include <unordered_set>

namespace zg::app {

LifecycleDelta phantom_lifecycle_diff(
    std::unordered_map<long long, double>& seen,
    const std::vector<zg::telemetry::Phantom>& current,
    double now_ts) {

    LifecycleDelta out;

    // First pass: build the set of current ids and emit new-indices for
    // anything that wasn't in `seen` last frame.
    std::unordered_set<long long> current_ids;
    current_ids.reserve(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) {
        const auto& ph = current[i];
        current_ids.insert(ph.id);
        if (seen.find(ph.id) == seen.end()) {
            seen[ph.id] = ph.spawn_time;
            out.new_indices.push_back(i);
        }
    }

    // Second pass: erase departed ids and append them to the delta with
    // their lifetime. Done in one pass with the iterator erase idiom so
    // we don't invalidate iterators while iterating.
    for (auto it = seen.begin(); it != seen.end();) {
        if (current_ids.find(it->first) == current_ids.end()) {
            out.departed.emplace_back(it->first, now_ts - it->second);
            it = seen.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

}  // namespace zg::app
