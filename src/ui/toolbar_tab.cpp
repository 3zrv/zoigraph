#include "ui/toolbar_tab.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <raylib.h>

#include <cmath>
#include <ctime>
#include <string>

#include "app/clock.h"
#include "graph/types.h"
#include "persistence/db.h"
#include "telemetry/phantom.h"

namespace zg::ui {

void render_toolbar_tab(zg::app::Session& s,
                        zg::telemetry::PhantomBuffer& phantom_buffer) {
    // Aliases shorten the rest of the function and match the names the
    // logic used when it lived in main.cpp's render loop.
    auto& db           = s.db;
    auto& physics      = s.physics;
    auto& stored_nodes = s.stored_nodes;
    auto& edges        = s.edges;
    auto& positions    = s.positions;
    auto& self_idx     = s.self_idx;
    auto& selected_node = s.selected_node;

    // ---- create static node ------------------------------------------
    static std::string tb_node_title;
    static std::string tb_node_msg;
    static int         tb_node_tier_idx = 0;
    static const char* kToolbarTiers[] = {"confirmed", "suspected", "phantom"};

    ImGui::TextDisabled("create static node");
    const bool node_submitted = ImGui::InputText("title##tb_node", &tb_node_title,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) tb_node_msg.clear();
    ImGui::Combo("tier##tb_node", &tb_node_tier_idx, kToolbarTiers, 3);

    // Optional auto-edge to the current selection so a manually-created
    // node doesn't have to land isolated (previously the only in-app ways
    // to a connected node were wikilinks and the journal). Greyed out when
    // nothing is selected. The edge is operator-created and therefore
    // certainty="confirmed"; kind stays empty -- editable afterwards in
    // the inspector's incident-edges list.
    static bool tb_node_link_sel = true;
    const bool can_link =
        selected_node >= 0 &&
        static_cast<std::size_t>(selected_node) < stored_nodes.size() &&
        static_cast<std::size_t>(selected_node) < positions.size() &&
        !stored_nodes[static_cast<std::size_t>(selected_node)].deleted;
    if (!can_link) ImGui::BeginDisabled();
    ImGui::Checkbox("edge to selection##tb_node", &tb_node_link_sel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("spawn the new node next to the selected one and\n"
                          "connect them (kind editable in the inspector).\n"
                          "needs a selected node.");
    }
    if (!can_link) ImGui::EndDisabled();

    if (ImGui::Button("create node") || node_submitted) {
        if (tb_node_title.empty()) {
            tb_node_msg = "title is empty";
        } else if (!physics || !db) {
            tb_node_msg = "no active project";
        } else {
            const double now_ts = zg::app::unix_now();
            const long long new_id = static_cast<long long>(stored_nodes.size());
            const bool link = tb_node_link_sel && can_link;
            const float angle = 0.7f * static_cast<float>(new_id);
            Vector3 spawn{
                8.0f * std::cos(angle),
                0.0f,
                8.0f * std::sin(angle),
            };
            if (link) {
                // Spawn beside the selected node (same offset pattern as
                // journal entries around self) so the new spring doesn't
                // yank it across the field on the first tick.
                const Vector3 a = positions[static_cast<std::size_t>(selected_node)];
                spawn = Vector3{
                    a.x + 3.0f * std::cos(angle),
                    a.y + 1.0f,
                    a.z + 3.0f * std::sin(angle),
                };
            }
            zg::persistence::StoredNode n{};
            n.id           = new_id;
            n.position     = spawn;
            n.title        = tb_node_title;
            n.content      = "";
            n.first_seen   = now_ts;
            n.last_touched = now_ts;
            n.tier         = kToolbarTiers[tb_node_tier_idx];
            stored_nodes.push_back(std::move(n));
            physics->enqueue_node(spawn);
            db->insert_node(stored_nodes.back());
            if (link) {
                const std::size_t sel = static_cast<std::size_t>(selected_node);
                const zg::graph::Edge e{
                    static_cast<std::size_t>(new_id), sel, "", "", "confirmed"};
                edges.push_back(e);
                physics->enqueue_edge(e);
                db->insert_edge(e);
                tb_node_msg = "added " + tb_node_title + " -> " +
                    (stored_nodes[sel].title.empty() ? "(untitled)"
                                                     : stored_nodes[sel].title);
            } else {
                tb_node_msg = "added " + tb_node_title;
            }
            tb_node_title.clear();
        }
    }
    if (!tb_node_msg.empty()) {
        ImGui::TextDisabled("%s", tb_node_msg.c_str());
    }

    ImGui::Separator();

    // ---- inject phantom ---------------------------------------------
    static std::string tb_phantom_label;
    static std::string tb_phantom_msg;
    static long long   tb_phantom_id_counter = 1000000;

    ImGui::TextDisabled("inject phantom");
    const bool phantom_submitted = ImGui::InputText("label##tb_phantom", &tb_phantom_label,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) tb_phantom_msg.clear();
    if (ImGui::Button("inject") || phantom_submitted) {
        zg::telemetry::Phantom p{};
        p.id         = tb_phantom_id_counter++;
        p.position   = {0.0f, 6.0f, 0.0f};
        p.label      = tb_phantom_label.empty() ? "(local)" : tb_phantom_label;
        p.spawn_time = GetTime();
        phantom_buffer.add(p);
        tb_phantom_msg = "injected id " + std::to_string(p.id);
        tb_phantom_label.clear();
    }
    if (!tb_phantom_msg.empty()) {
        ImGui::TextDisabled("%s", tb_phantom_msg.c_str());
    }

    ImGui::Separator();

    // ---- journal entry ----------------------------------------------
    // Timestamped first-class node tagged "journal", auto-edged from
    // self (kind 'wrote') and from the currently-selected node if any
    // (kind 'concerns'). Memex trails by accident.
    static std::string tb_journal_text;
    static std::string tb_journal_msg;
    ImGui::TextDisabled("journal entry");
    ImGui::InputTextMultiline("##tb_journal", &tb_journal_text,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
    if (ImGui::IsItemEdited()) tb_journal_msg.clear();
    if (ImGui::Button("save journal")) {
        if (tb_journal_text.empty()) {
            tb_journal_msg = "entry is empty";
        } else if (!physics || !db) {
            tb_journal_msg = "no active project";
        } else {
            const double now_ts = zg::app::unix_now();
            const std::time_t tt = static_cast<std::time_t>(now_ts);
            char tbuf[32];
            std::strftime(tbuf, sizeof(tbuf), "journal-%Y%m%d-%H%M%S",
                          std::localtime(&tt));
            const long long new_id = static_cast<long long>(stored_nodes.size());
            const float ang = 0.5f * static_cast<float>(new_id);
            Vector3 anchor{0.0f, 0.0f, 0.0f};
            if (self_idx < positions.size()) anchor = positions[self_idx];
            const Vector3 spawn{
                anchor.x + 3.0f * std::cos(ang),
                anchor.y + 1.0f + 0.3f * static_cast<float>(new_id % 5),
                anchor.z + 3.0f * std::sin(ang),
            };
            zg::persistence::StoredNode jn{};
            jn.id           = new_id;
            jn.position     = spawn;
            jn.title        = tbuf;
            jn.content      = tb_journal_text;
            jn.first_seen   = now_ts;
            jn.last_touched = now_ts;
            jn.tier         = "confirmed";
            jn.tags         = {"journal"};
            stored_nodes.push_back(std::move(jn));
            physics->enqueue_node(spawn);
            db->insert_node(stored_nodes.back());

            if (self_idx < stored_nodes.size()) {
                const zg::graph::Edge e_self{
                    self_idx, static_cast<std::size_t>(new_id),
                    "wrote", "knows", "confirmed"};
                edges.push_back(e_self);
                physics->enqueue_edge(e_self);
                db->insert_edge(e_self);
            }
            if (selected_node >= 0 &&
                static_cast<std::size_t>(selected_node) < stored_nodes.size() &&
                static_cast<std::size_t>(selected_node) != static_cast<std::size_t>(new_id)) {
                const zg::graph::Edge e_about{
                    static_cast<std::size_t>(new_id),
                    static_cast<std::size_t>(selected_node),
                    "concerns", "saw-at", "confirmed"};
                edges.push_back(e_about);
                physics->enqueue_edge(e_about);
                db->insert_edge(e_about);
            }

            tb_journal_msg = "saved " + std::string(tbuf);
            tb_journal_text.clear();
        }
    }
    if (!tb_journal_msg.empty()) {
        ImGui::TextDisabled("%s", tb_journal_msg.c_str());
    }
}

}  // namespace zg::ui
