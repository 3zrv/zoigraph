#include "persistence/seed.h"

#include <random>
#include <string>

namespace zg::persistence {

namespace {

// Small fixed pools so output stays reproducible for tests and the visual
// flavor is consistent across runs.
const char* const kCodenames[] = {
    "alpha",   "vector", "raven",   "drift",  "lattice", "cipher",
    "ember",   "halcyon","quartz",  "specter","vagrant", "wraith",
    "binder",  "ferrite","marrow",  "obelisk","static",  "umbra",
    "kestrel", "skiff",  "stylus",  "talon",  "amber",   "nimbus",
};
constexpr int kCodenamesN = sizeof(kCodenames) / sizeof(kCodenames[0]);

const char* const kTagPool[] = {
    "subject", "asset", "front", "hostile",
    "deceased", "informant", "surveillance", "lead",
};
constexpr int kTagPoolN = sizeof(kTagPool) / sizeof(kTagPool[0]);

const char* const kContentTemplates[] = {
    "intel inbound — needs corroboration",
    "last known contact: peripheral",
    "flagged by routine pattern match",
    "cross-reference with prior incident",
    "unverified third-party report",
    "telemetry only — no human source",
};
constexpr int kContentTemplatesN = sizeof(kContentTemplates) / sizeof(kContentTemplates[0]);

}  // namespace

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
                              unsigned int rng_seed,
                              bool         with_data) {
    InitialGraph out;
    if (node_count <= 0) return out;

    std::mt19937 rng(rng_seed);
    std::uniform_real_distribution<float> pos(-spread, spread);
    std::uniform_int_distribution<int>    codename(0, kCodenamesN - 1);
    std::uniform_int_distribution<int>    content_pick(0, kContentTemplatesN - 1);
    std::uniform_int_distribution<int>    tag_pick(0, kTagPoolN - 1);
    std::uniform_int_distribution<int>    tag_count(0, 3);

    out.nodes.reserve(node_count);
    for (int i = 0; i < node_count; ++i) {
        StoredNode n{};
        n.id           = start_id + i;
        n.position     = {pos(rng), pos(rng), pos(rng)};
        n.first_seen   = now_unix;
        n.last_touched = now_unix;
        n.tier         = "confirmed";

        if (with_data) {
            n.title   = std::string(kCodenames[codename(rng)]) + "-" + std::to_string(n.id);
            n.content = kContentTemplates[content_pick(rng)];
            const int tn = tag_count(rng);
            for (int t = 0; t < tn; ++t) {
                const std::string tag = kTagPool[tag_pick(rng)];
                // dedup within this node so the chip row matches DB state.
                bool dup = false;
                for (const auto& existing : n.tags) {
                    if (existing == tag) { dup = true; break; }
                }
                if (!dup) n.tags.push_back(tag);
            }
        }

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
