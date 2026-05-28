#pragma once

#include <raylib.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/ask.h"
#include "graph/graph_buffer.h"
#include "graph/types.h"
#include "persistence/db.h"
#include "physics/physics_thread.h"
#include "telemetry/phantom_buffer.h"

namespace zg::app {

// All per-project state. One instance lives in main(); open_project()
// repopulates it when the operator switches projects. Stuff that's
// process-scoped (window, shaders, render texture, telemetry thread,
// the GraphBuffer / PhantomBuffer themselves) stays in main.
struct Session {
    std::unique_ptr<zg::persistence::Database>    db;
    std::unique_ptr<zg::physics::PhysicsThread>   physics;
    LlmAsk                                        ask;          // "Ask about selection" worker; process-scoped behaviour, parked here for inspector access

    std::vector<zg::persistence::StoredNode>      stored_nodes;
    std::vector<zg::graph::Edge>                  edges;
    std::vector<Vector3>                          positions;

    std::vector<std::size_t>                      cluster_ids;     // empty == no clustering active
    std::string                                   tag_filter;      // empty == no filter
    std::size_t                                   self_idx       = SIZE_MAX;
    bool                                          timeline_mode  = false;
    double                                        prev_open_ts   = 0.0;
    std::string                                   current_project;

    int                                           selected_node  = -1;
    std::string                                   search_query;
    std::vector<long long>                        search_hits;
};

// Tears down any in-progress session (stop physics, final save_graph,
// release the DB), then opens `name` against the projects_dir:
//   - loads the persisted graph if the DB has one
//   - otherwise seeds a fresh project (self + alice + bob + 300 random
//     nodes + 200 random edges)
// Wires the new physics thread to the supplied buffer and phantom_buffer.
// Pins the self node, snapshots prev_open_ts and bumps last_open_ts,
// resets timeline mode and selection.
void open_project(Session& s,
                  const std::string& name,
                  const std::filesystem::path& projects_dir,
                  zg::graph::GraphBuffer& buffer,
                  zg::telemetry::PhantomBuffer& phantom_buffer);

}  // namespace zg::app
