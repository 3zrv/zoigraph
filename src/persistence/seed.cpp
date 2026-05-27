#include "persistence/seed.h"

#include <random>

namespace zg::persistence {

InitialGraph make_initial_graph(double now_unix) {
    InitialGraph out;
    out.nodes.reserve(3);

    StoredNode self_n{};
    self_n.id           = 0;
    self_n.position     = {0.0f, 0.0f, 0.0f};
    self_n.title        = "self";
    self_n.content      = "the operator";
    self_n.first_seen   = now_unix;
    self_n.last_touched = now_unix;
    self_n.tier         = "self";
    out.nodes.push_back(std::move(self_n));

    StoredNode alice{};
    alice.id           = 1;
    alice.position     = {-5.0f, 0.0f, 0.0f};
    alice.title        = "alice";
    alice.content      = "";
    alice.first_seen   = now_unix;
    alice.last_touched = now_unix;
    alice.tier         = "confirmed";
    out.nodes.push_back(std::move(alice));

    StoredNode bob{};
    bob.id           = 2;
    bob.position     = {5.0f, 0.0f, 0.0f};
    bob.title        = "bob";
    bob.content      = "";
    bob.first_seen   = now_unix;
    bob.last_touched = now_unix;
    bob.tier         = "confirmed";
    out.nodes.push_back(std::move(bob));

    return out;
}

InitialGraph make_random_fill(int          node_count,
                              int          edge_count,
                              long long    start_id,
                              double       now_unix,
                              float        spread,
                              unsigned int rng_seed) {
    InitialGraph out;
    if (node_count <= 0) return out;

    std::mt19937 rng(rng_seed);
    std::uniform_real_distribution<float> pos(-spread, spread);

    out.nodes.reserve(node_count);
    for (int i = 0; i < node_count; ++i) {
        StoredNode n{};
        n.id           = start_id + i;
        n.position     = {pos(rng), pos(rng), pos(rng)};
        n.title        = "";
        n.content      = "";
        n.first_seen   = now_unix;
        n.last_touched = now_unix;
        n.tier         = "confirmed";
        out.nodes.push_back(std::move(n));
    }

    if (edge_count > 0 && node_count >= 2) {
        std::uniform_int_distribution<std::size_t> idx(
            static_cast<std::size_t>(start_id),
            static_cast<std::size_t>(start_id + node_count - 1));
        out.edges.reserve(edge_count);
        for (int i = 0; i < edge_count; ++i) {
            std::size_t a = idx(rng);
            std::size_t b = idx(rng);
            while (b == a) b = idx(rng);
            out.edges.push_back({a, b});
        }
    }

    return out;
}

}  // namespace zg::persistence
