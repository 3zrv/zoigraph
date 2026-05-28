#pragma once

#include <cstddef>
#include <set>
#include <vector>

#include <raylib.h>

#include "graph/types.h"
#include "persistence/db.h"
#include "telemetry/phantom.h"

namespace zg::render {

// Compute the set of node indices whose titles should be rendered this
// frame. Pure logic, no Raylib calls -- the renderer below consumes the
// returned set.
//
// In-focus set is the union of:
//   - selected (if valid and not tombstoned)
//   - every 1-hop neighbour of selected reachable via `edges` (either
//     direction)
//   - every phantom-connection target (across all `phantoms`)
//   - every index in `bones_chosen`
//
// If `always_show` is true, the result is every non-deleted index in
// [0, stored_nodes.size()) regardless of the other inputs -- the
// L-toggle overview mode.
//
// All sources drop indices that are out of range or refer to tombstoned
// (stored_nodes[i].deleted) nodes. Negative phantom target ids are
// dropped.
std::set<std::size_t> compute_label_set(
    int selected,
    const std::vector<zg::graph::Edge>& edges,
    const std::vector<zg::telemetry::Phantom>& phantoms,
    const std::vector<std::size_t>& bones_chosen,
    const std::vector<zg::persistence::StoredNode>& stored_nodes,
    bool always_show);

// Draws each label at the screen-projected position of its node, with a
// small Y offset so the text floats above the sphere. Labels for nodes
// projecting behind the camera are silently skipped.
void draw_node_labels(
    const std::set<std::size_t>& indices,
    const std::vector<Vector3>& positions,
    const std::vector<zg::persistence::StoredNode>& stored_nodes,
    const Camera3D& camera);

}  // namespace zg::render
