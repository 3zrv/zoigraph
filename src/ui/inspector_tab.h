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
//
// Mutates session.{stored_nodes, edges, selected_node, search_*} as the
// operator clicks; also mutates camera when the search box jumps to a hit.
void render_inspector_tab(zg::app::Session& session,
                          Camera3D& camera,
                          const std::vector<zg::telemetry::Phantom>& phantoms,
                          const zg::telemetry::TelemetryThread& telemetry);

}  // namespace zg::ui
