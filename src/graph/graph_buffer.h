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

    // Consumer side (render thread). Copies the latest snapshot.
    void snapshot(std::vector<Vector3>& out_positions, std::vector<Edge>& out_edges) const;

private:
    mutable std::mutex mu_;
    std::vector<Vector3> positions_;
    std::vector<Edge>    edges_;
};

}  // namespace zg::graph
