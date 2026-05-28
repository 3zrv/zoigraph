#include "app/promote.h"

namespace zg::app {

PromotionResult promote_phantom(const zg::telemetry::Phantom& ph,
                                long long new_id,
                                double now_ts,
                                std::size_t positions_size) {
    PromotionResult out;

    out.node.id           = new_id;
    out.node.position     = ph.position;
    out.node.title        = ph.label;
    out.node.content      = ph.content;
    out.node.first_seen   = now_ts;
    out.node.last_touched = now_ts;
    out.node.tier         = "phantom";

    out.edges.reserve(ph.connections.size());
    for (long long target_id : ph.connections) {
        if (target_id < 0) continue;
        const auto tidx = static_cast<std::size_t>(target_id);
        if (tidx >= positions_size) continue;
        if (tidx == static_cast<std::size_t>(new_id)) continue;
        out.edges.push_back(zg::graph::Edge{
            static_cast<std::size_t>(new_id), tidx,
            /*label=*/"", /*kind=*/"", /*certainty=*/"phantom"});
    }
    return out;
}

}  // namespace zg::app
