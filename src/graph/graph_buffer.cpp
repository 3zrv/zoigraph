#include "graph/graph_buffer.h"

namespace zg::graph {

void GraphBuffer::set_edges(std::vector<Edge> edges) {
    std::lock_guard<std::mutex> lock(mu_);
    edges_ = std::move(edges);
}

void GraphBuffer::publish_positions(const std::vector<Vector3>& positions) {
    std::lock_guard<std::mutex> lock(mu_);
    positions_ = positions;
}

void GraphBuffer::snapshot(std::vector<Vector3>& out_positions, std::vector<Edge>& out_edges) const {
    std::lock_guard<std::mutex> lock(mu_);
    out_positions = positions_;
    out_edges     = edges_;
}

void GraphBuffer::snapshot(std::vector<Vector3>& out_positions) const {
    std::lock_guard<std::mutex> lock(mu_);
    out_positions = positions_;
}

}  // namespace zg::graph
