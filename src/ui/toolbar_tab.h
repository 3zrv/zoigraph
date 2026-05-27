#pragma once

#include "app/session.h"
#include "telemetry/phantom_buffer.h"

namespace zg::ui {

// Renders the contents of the "Toolbar" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem). Three forms:
//   - create static node (title + tier + Create)
//   - inject phantom    (label + Inject)
//   - journal entry     (multi-line text + Save)
//
// Mutates the session as the operator clicks: new StoredNodes are
// appended, edges are pushed (for journal -> self / selected), the DB
// is updated immediately, and the physics thread receives enqueues.
void render_toolbar_tab(zg::app::Session& session,
                        zg::telemetry::PhantomBuffer& phantom_buffer);

}  // namespace zg::ui
