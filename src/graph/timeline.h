#pragma once

#include <raylib.h>

#include <vector>

namespace zg::graph {

// Given a list of per-node `first_seen` timestamps (Unix seconds, 0 ==
// unknown), returns positions for each node that lay them out on a
// horizontal timeline:
//   x      = linear interpolation across the live range of timestamps,
//            spanning [-x_span, +x_span]. Nodes with first_seen == 0
//            are treated as the earliest known time.
//   (y,z)  = column packing in the y-z plane perpendicular to the
//            timeline axis. When two or more nodes share an x within
//            an internal tolerance, they radiate from (0,0) along a
//            Vogel's-spiral (golden-angle increment, radius scaled by
//            sqrt(slot) * `column_spacing`) so a 50-node cluster
//            forms a disc rather than a 150-unit-tall pillar. Single-
//            node columns get a deterministic hash-based jitter in
//            y ∈ [-y_jitter, +y_jitter] instead, with z = 0.
//
// Edge cases:
//   - empty input -> empty output
//   - single node -> position at origin
//   - all timestamps equal (or all zero) -> nodes spaced evenly along x
//     by index instead so the operator still sees them
std::vector<Vector3> compute_timeline_positions(
    const std::vector<double>& first_seens,
    float x_span         = 60.0f,
    float y_jitter       = 10.0f,
    float column_spacing = 3.0f);

}  // namespace zg::graph
