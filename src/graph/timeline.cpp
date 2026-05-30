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

// Deterministic [-magnitude, +magnitude] jitter keyed by (i, seed).
// Different seeds give independent jitter axes -- we use one for y and
// one for z so single-node columns get full 3D scatter instead of a
// flat y-only ribbon.
float hash_jitter(std::size_t i, float magnitude, std::size_t seed) {
    const std::size_t h = std::hash<std::size_t>{}(i * seed);
    const float frac = static_cast<float>(h & 0xFFFFu) / 65535.0f;
    return -magnitude + 2.0f * magnitude * frac;
}

constexpr std::size_t kHashSeedY = 2654435761u;
constexpr std::size_t kHashSeedZ = 1442695040u;

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

    // Pass 3: assign (y, z) per column. Single-node columns keep the
    // legacy y-only hash jitter so distinct-timestamp layouts don't all
    // collapse onto the axis. Multi-node columns radiate from (0,0) in
    // the y-z plane along a Vogel's-spiral -- golden-angle steps with
    // radius growing as sqrt(slot) -- so the column becomes a disc, not
    // a tall vertical line. Slot 0 always lands at the centre.
    constexpr float kGoldenAngleRad = 2.39996322972865332f;
    for (auto& col : columns) {
        // Sort by original index so slot assignment is deterministic
        // across libc++/libstdc++ stable-sort differences.
        std::sort(col.begin(), col.end());
        const std::size_t k = col.size();
        if (k == 1) {
            const std::size_t idx = col.front();
            // Two independent hash axes -- a y-only jitter would leave
            // single-node columns flat in the x-y plane, which is what
            // the operator sees as a "ribbon" when their corpus has no
            // real temporal info (every node falls in its own column).
            out[idx] = {
                xs[idx],
                hash_jitter(idx, y_jitter, kHashSeedY),
                hash_jitter(idx, y_jitter, kHashSeedZ),
            };
            continue;
        }
        for (std::size_t slot = 0; slot < k; ++slot) {
            const std::size_t idx    = col[slot];
            const float       angle  = static_cast<float>(slot) * kGoldenAngleRad;
            const float       radius = column_spacing
                                       * std::sqrt(static_cast<float>(slot));
            out[idx] = {
                xs[idx],
                radius * std::cos(angle),
                radius * std::sin(angle),
            };
        }
    }
    return out;
}

}  // namespace zg::graph
