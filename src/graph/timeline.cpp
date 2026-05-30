#include "graph/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace zg::graph {

namespace {

// Two xs within this many worldspace units are treated as the same
// column for stacking purposes. Small enough that a year-apart pair on
// a 60-unit half-span stays separated; large enough that month-apart
// observations stack instead of overlapping.
constexpr float kColumnXTolerance = 0.5f;

float hash_jitter(std::size_t i, float y_jitter) {
    const std::size_t h = std::hash<std::size_t>{}(i * 2654435761u);
    const float frac = static_cast<float>(h & 0xFFFFu) / 65535.0f;
    return -y_jitter + 2.0f * y_jitter * frac;
}

}  // namespace

std::vector<Vector3> compute_timeline_positions(
    const std::vector<double>& first_seens,
    float x_span,
    float y_jitter,
    float column_spacing) {
    const std::size_t n = first_seens.size();
    std::vector<Vector3> out(n, Vector3{0.0f, 0.0f, 0.0f});
    if (n == 0) return out;
    if (n == 1) return out;

    double t_min = 0.0;
    double t_max = 0.0;
    bool   any   = false;
    for (double t : first_seens) {
        if (t <= 0.0) continue;
        if (!any) { t_min = t_max = t; any = true; }
        else {
            if (t < t_min) t_min = t;
            if (t > t_max) t_max = t;
        }
    }

    const bool all_equal = !any || (t_max - t_min) < 1e-9;

    // Pass 1: compute x for each node, same as before.
    std::vector<float> xs(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        if (all_equal) {
            const float frac = (n > 1)
                ? static_cast<float>(i) / static_cast<float>(n - 1)
                : 0.5f;
            xs[i] = -x_span + 2.0f * x_span * frac;
        } else {
            const double t = (first_seens[i] > 0.0) ? first_seens[i] : t_min;
            const double frac = (t - t_min) / (t_max - t_min);
            xs[i] = -x_span + 2.0f * x_span * static_cast<float>(frac);
        }
    }

    // Pass 2: walk indices in ascending-x order and group adjacent ones
    // that fall within kColumnXTolerance into a column. Stable on equal
    // xs because std::stable_sort preserves ascending-index order, so
    // slot assignment within a column is deterministic by index.
    std::vector<std::size_t> by_x(n);
    for (std::size_t i = 0; i < n; ++i) by_x[i] = i;
    std::stable_sort(by_x.begin(), by_x.end(),
        [&xs](std::size_t a, std::size_t b) { return xs[a] < xs[b]; });

    std::vector<std::vector<std::size_t>> columns;
    for (std::size_t idx : by_x) {
        if (columns.empty()
            || (xs[idx] - xs[columns.back().front()]) > kColumnXTolerance) {
            columns.push_back({idx});
        } else {
            columns.back().push_back(idx);
        }
    }

    // Pass 3: assign y per column. Single-node columns keep the legacy
    // hash-based jitter so distinct-timestamp layouts don't all collapse
    // onto the axis. Multi-node columns stack at deterministic slots
    // centred on y=0 with neighbours `column_spacing` apart.
    for (auto& col : columns) {
        // Re-sort by original index within the column so slot order is
        // independent of the by_x sort stability quirks across libc++/libstdc++.
        std::sort(col.begin(), col.end());
        const std::size_t k = col.size();
        if (k == 1) {
            const std::size_t idx = col.front();
            out[idx] = {xs[idx], hash_jitter(idx, y_jitter), 0.0f};
        } else {
            const float center_offset = 0.5f * static_cast<float>(k - 1);
            for (std::size_t slot = 0; slot < k; ++slot) {
                const std::size_t idx = col[slot];
                const float y = (static_cast<float>(slot) - center_offset)
                                * column_spacing;
                out[idx] = {xs[idx], y, 0.0f};
            }
        }
    }
    return out;
}

}  // namespace zg::graph
