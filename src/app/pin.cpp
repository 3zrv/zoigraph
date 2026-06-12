#include "app/pin.h"

#include <raylib.h>

#include <nlohmann/json.hpp>

#include <utility>

#include "app/clock.h"
#include "app/promote.h"
#include "persistence/db.h"

namespace zg::app {

long long pin_phantom(Session& s,
                      const zg::telemetry::Phantom& ph,
                      zg::telemetry::PhantomBuffer& phantom_buffer,
                      std::unordered_map<long long, double>& seen_phantom_spawn) {
    const long long new_id      = static_cast<long long>(s.stored_nodes.size());
    const double    promoted_ts = unix_now();

    // promote_phantom is the pure-logic version of click-to-pin (covered
    // by test_promote). Edge dropping rules and the trust-gradient
    // defaults (node tier + edge certainty = "phantom") live there; this
    // site just consumes the result.
    auto promo = promote_phantom(ph, new_id, promoted_ts, s.positions.size());
    s.stored_nodes.push_back(std::move(promo.node));
    s.db->insert_node(s.stored_nodes.back());
    for (const auto& e : promo.edges) {
        s.edges.push_back(e);
        if (s.physics) s.physics->enqueue_edge(e);
        s.db->insert_edge(e);
    }

    phantom_buffer.remove(ph.id);
    if (s.physics) s.physics->enqueue_node(ph.position);
    s.selected_node = static_cast<int>(new_id);

    // Log the pin and remove this id from the spawn tracker so main's
    // per-frame diff doesn't also fire a decay event for the same
    // phantom. time_to_pin_s is the wall-clock gap between the spawn UDP
    // packet landing and the operator accepting; it's the headline
    // behavioural signal for the trust-gradient test.
    const auto spawn_it = seen_phantom_spawn.find(ph.id);
    const double time_to_pin = (spawn_it != seen_phantom_spawn.end())
        ? (GetTime() - spawn_it->second) : 0.0;
    if (spawn_it != seen_phantom_spawn.end()) {
        seen_phantom_spawn.erase(spawn_it);
    }
    nlohmann::json conns = nlohmann::json::array();
    for (const auto& c : ph.connections) {
        conns.push_back({{"target", c.target}, {"kind", c.kind}});
    }
    nlohmann::json payload = {
        {"phantom_id",    ph.id},
        {"new_node_id",   new_id},
        {"label",         ph.label},
        {"content",       ph.content},
        {"connections",   conns},
        {"time_to_pin_s", time_to_pin},
        {"source",        ph.source},
    };
    s.db->log_event("phantom_pin", new_id, payload.dump());

    return new_id;
}

}  // namespace zg::app
