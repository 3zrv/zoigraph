#include "graph/timeline.h"

#include <algorithm>
#include <functional>

namespace zg::graph {

std::vector<Vector3> compute_timeline_positions(
    const std::vector<double>& first_seens,
    float x_span,
    float y_jitter) {
    const std::size_t n = first_seens.size();
    std::vector<Vector3> out(n, Vector3{0.0f, 0.0f, 0.0f});
    if (n == 0) return out;
    if (n == 1) return out;  // already at origin

    double t_min = 0.0;
    double t_max = 0.0;
    bool   any   = false;
    for (double t : first_seens) {
        if (t <= 0.0) continue;  // unknown
        if (!any) {
            t_min = t_max = t;
            any   = true;
        } else {
            if (t < t_min) t_min = t;
            if (t > t_max) t_max = t;
        }
    }

    const bool all_equal = !any || (t_max - t_min) < 1e-9;

    for (std::size_t i = 0; i < n; ++i) {
        float x;
        if (all_equal) {
            // No usable timestamps -> spread by index across the span.
            const float frac = (n > 1)
                ? static_cast<float>(i) / static_cast<float>(n - 1)
                : 0.5f;
            x = -x_span + 2.0f * x_span * frac;
        } else {
            const double t = (first_seens[i] > 0.0) ? first_seens[i] : t_min;
            const double frac = (t - t_min) / (t_max - t_min);
            x = -x_span + 2.0f * x_span * static_cast<float>(frac);
        }

        // y jitter: hash of index mapped into [-y_jitter, +y_jitter].
        const std::size_t h = std::hash<std::size_t>{}(i * 2654435761u);
        const float frac_y  = (static_cast<float>(h & 0xFFFFu) / 65535.0f);
        const float y       = -y_jitter + 2.0f * y_jitter * frac_y;

        out[i] = {x, y, 0.0f};
    }
    return out;
}

}  // namespace zg::graph
