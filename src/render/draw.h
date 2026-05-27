#pragma once

#include <raylib.h>

namespace zg::render {

// Animated jagged line: subdivides the straight segment a->b into 8
// pieces and perturbs each interior point perpendicular to the line
// using sin/cos driven by `seed` and a wall-clock `time` term, so the
// line visibly jitters ("erratic" per directive §5.B). Used for the
// phantom-to-static connections.
void draw_jagged_line(Vector3 a, Vector3 b, Color color, double time, long long seed);

}  // namespace zg::render
