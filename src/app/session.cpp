#include "app/session.h"

#include <stdexcept>
#include <string>

#include "app/clock.h"
#include "persistence/db.h"
#include "persistence/project_store.h"
#include "persistence/seed.h"

namespace zg::app {

long long Session::next_node_id() const {
    return static_cast<long long>(stored_nodes.size());
}

long long Session::append_node(zg::persistence::StoredNode n) {
    const long long id = next_node_id();
    n.id = id;
    stored_nodes.push_back(std::move(n));
    if (physics) physics->enqueue_node(stored_nodes.back().position);
    if (db)      db->insert_node(stored_nodes.back());
    return id;
}

void open_project(Session& s,
                  const std::string& name,
                  const std::filesystem::path& projects_dir,
                  zg::graph::GraphBuffer& buffer,
                  zg::telemetry::PhantomBuffer& phantom_buffer) {
    // Save and tear down the current session if there is one.
    if (s.physics) {
        s.physics->stop();
        std::vector<Vector3> final_positions;
        buffer.snapshot(final_positions);
        for (std::size_t i = 0; i < final_positions.size() && i < s.stored_nodes.size(); ++i) {
            s.stored_nodes[i].position = final_positions[i];
        }
        if (s.db) s.db->save_graph(s.stored_nodes, s.edges);
        s.physics.reset();
    }
    s.db.reset();
    phantom_buffer.clear();
    s.stored_nodes.clear();
    s.edges.clear();
    s.positions.clear();
    s.cluster_ids.clear();
    s.tag_filter.clear();
    s.selected_node = -1;
    s.search_query.clear();
    s.search_hits.clear();

    s.current_project = name;
    zg::persistence::write_last_project(projects_dir, name);
    s.db = std::make_unique<zg::persistence::Database>(
        zg::persistence::project_path(projects_dir, name).string());

    std::vector<zg::graph::Edge> initial_edges;
    std::vector<Vector3>         initial_positions;
    if (s.db->load_graph(s.stored_nodes, initial_edges)) {
        // id == vector index is load-bearing: edges store source/target as
        // indices into stored_nodes, not ids. An externally-edited DB with a
        // gap or out-of-order id would silently mis-point every edge and then
        // crash on the next pin (PK collision). Refuse to open instead — no
        // silent remap (that belongs to a future import/merge path).
        if (std::size_t bad = zg::persistence::first_noncontiguous_id(s.stored_nodes);
            bad != s.stored_nodes.size()) {
            throw std::runtime_error(
                "project '" + name + "': node ids must be contiguous 0..N-1 "
                "(id==index is load-bearing). First mismatch at position "
                + std::to_string(bad) + " has id "
                + std::to_string(s.stored_nodes[bad].id)
                + " — the DB was likely edited externally.");
        }
        initial_positions.reserve(s.stored_nodes.size());
        for (const auto& sn : s.stored_nodes) initial_positions.push_back(sn.position);
    } else {
        // Fresh project: just the three named seed nodes (self/alice/bob).
        // No random bulk fill — operator grows the graph by hand.
        auto seed = zg::persistence::make_initial_graph(unix_now());
        s.stored_nodes = std::move(seed.nodes);
        initial_edges  = std::move(seed.edges);
        initial_positions.reserve(s.stored_nodes.size());
        for (const auto& sn : s.stored_nodes) initial_positions.push_back(sn.position);
    }
    // Seed main's render-loop state from what's about to be handed to physics.
    s.positions = initial_positions;
    s.edges     = initial_edges;

    // Find the self node (tier == "self") so touch-edges and the self-pin
    // both have a stable anchor.
    s.self_idx = SIZE_MAX;
    for (std::size_t i = 0; i < s.stored_nodes.size(); ++i) {
        if (s.stored_nodes[i].tier == "self") {
            s.self_idx = i;
            break;
        }
    }

    s.physics = std::make_unique<zg::physics::PhysicsThread>(
        std::move(initial_positions),
        initial_edges,
        buffer,
        &phantom_buffer);
    s.physics->start();

    if (s.self_idx < s.positions.size()) {
        s.physics->set_pin(s.self_idx, s.positions[s.self_idx]);
    }

    s.prev_open_ts = s.db->meta_double("last_open_ts", 0.0);
    s.db->set_meta_double("last_open_ts", unix_now());

    s.timeline_mode = false;
}

}  // namespace zg::app
