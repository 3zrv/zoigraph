#pragma once

#include <raylib.h>

#include <mutex>
#include <vector>

#include "graph/types.h"

namespace zg::graph {

// Single-writer / single-reader handoff for node positions and the edge list.
// Edges are write-once at startup and then read-only; positions are pushed each
// physics tick. A single mutex is fine at the milestone-1 scale (N=500).
class GraphBuffer {
public:
    void set_edges(std::vector<Edge> edges);

    // Producer side (physics thread). Caller must size `positions` to N.
    void publish_positions(const std::vector<Vector3>& positions);

    // Consumer side (render thread). Copies the latest snapshot of both
    // positions and edges. Used by tests; the running app uses the
    // positions-only overload below because edge metadata (label / kind /
    // certainty) is owned by main.cpp and would be clobbered if read back
    // from the buffer each frame.
    void snapshot(std::vector<Vector3>& out_positions, std::vector<Edge>& out_edges) const;

    // Positions-only snapshot. Doesn't touch the caller's edges vector at
    // all — leave edge ownership to whoever maintains the metadata.
    void snapshot(std::vector<Vector3>& out_positions) const;

private:
    mutable std::mutex mu_;
    std::vector<Vector3> positions_;
    std::vector<Edge>    edges_;
};

}  // namespace zg::graph
