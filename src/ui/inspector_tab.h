#pragma once

#include <raylib.h>

#include <vector>

#include "app/session.h"
#include "telemetry/phantom.h"
#include "telemetry/telemetry_thread.h"

namespace zg::ui {

// Renders the contents of the "Inspector" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem). Sections:
//   - stats (nodes / edges / phantoms / fps)
//   - FTS5 search box (jumps camera + selection to top hit)
//   - selected node editor (tier / tags / timestamps / title / content,
//     touch-edges + wikilinks-on-edit, save-to-disk)
//   - incident edges editor (label / kind / certainty)
//   - view filters (tag combo, auto-cluster, show_grid, CRT, Barnes-Hut)
//
// Mutates session.{stored_nodes, edges, positions, selected_node, search_*,
// tag_filter, cluster_ids} as the operator clicks; also mutates camera
// when the search box jumps to a hit, and the two view bools.
void render_inspector_tab(zg::app::Session& session,
                          Camera3D& camera,
                          const std::vector<zg::telemetry::Phantom>& phantoms,
                          const zg::telemetry::TelemetryThread& telemetry,
                          bool& show_grid,
                          bool& post_process);

}  // namespace zg::ui
