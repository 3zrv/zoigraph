#include "ui/inspector_tab.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <raylib.h>
#include <raymath.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "app/clock.h"
#include "graph/types.h"
#include "graph/wikilinks.h"
#include "persistence/db.h"

namespace zg::ui {

void render_inspector_tab(zg::app::Session& s,
                          Camera3D& camera,
                          const std::vector<zg::telemetry::Phantom>& phantoms,
                          const zg::telemetry::TelemetryThread& telemetry) {
    auto& db            = s.db;
    auto& physics       = s.physics;
    auto& stored_nodes  = s.stored_nodes;
    auto& edges         = s.edges;
    auto& positions     = s.positions;
    auto& self_idx      = s.self_idx;
    auto& selected_node = s.selected_node;
    auto& search_query  = s.search_query;
    auto& search_hits   = s.search_hits;

    ImGui::Text("nodes    %d", static_cast<int>(positions.size()));
    ImGui::Text("edges    %d", static_cast<int>(edges.size()));
    ImGui::Text("phantoms %d %s", static_cast<int>(phantoms.size()),
                telemetry.listening() ? "(udp :7777)" : "(listener off)");
    ImGui::Text("fps      %d", GetFPS());
    ImGui::Separator();

    // Real-time FTS5 search: on each keystroke, re-query the DB and jump
    // camera + selection to the top hit. The DB save_graph happens on
    // edits/shutdown, so the index reflects the on-disk state — local
    // unsaved edits to title/content won't surface until "save to disk."
    if (ImGui::InputTextWithHint("search", "title or content", &search_query)) {
        search_hits = db->search(search_query);
        if (!search_hits.empty()) {
            const auto idx = static_cast<std::size_t>(search_hits.front());
            if (idx < positions.size()) {
                selected_node = static_cast<int>(idx);
                const Vector3 offset = Vector3Subtract(camera.position, camera.target);
                camera.target = positions[idx];
                camera.position = Vector3Add(camera.target, offset);
            }
        }
    }
    if (!search_query.empty()) {
        ImGui::TextDisabled("%d match%s",
                            static_cast<int>(search_hits.size()),
                            search_hits.size() == 1 ? "" : "es");
    }
    ImGui::Separator();
    if (selected_node >= 0 && static_cast<std::size_t>(selected_node) < positions.size()
        && static_cast<std::size_t>(selected_node) < stored_nodes.size()) {
        const Vector3 p = positions[selected_node];
        ImGui::Text("node %d   pos %+6.1f %+6.1f %+6.1f", selected_node, p.x, p.y, p.z);
        ImGui::Spacing();

        auto& sn = stored_nodes[selected_node];

        // Tier picker: drives halo color in the 3D view and persists
        // immediately on change (no save-to-disk required).
        static const char* kTiers[] = {"confirmed", "suspected", "phantom", "self"};
        int tier_idx = 0;
        for (int t = 0; t < 4; ++t) {
            if (sn.tier == kTiers[t]) { tier_idx = t; break; }
        }
        if (ImGui::Combo("tier", &tier_idx, kTiers, 4)) {
            sn.tier = kTiers[tier_idx];
            db->update_node_tier(sn.id, sn.tier);
        }

        // Tag chips: each existing tag renders as a small "<tag> x"
        // button. Clicking removes the tag and persists the new set.
        ImGui::TextDisabled("tags");
        int remove_idx = -1;
        for (std::size_t ti = 0; ti < sn.tags.size(); ++ti) {
            ImGui::PushID(static_cast<int>(ti));
            const std::string chip = sn.tags[ti] + " x";
            if (ImGui::SmallButton(chip.c_str())) {
                remove_idx = static_cast<int>(ti);
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (remove_idx >= 0) {
            sn.tags.erase(sn.tags.begin() + remove_idx);
            db->update_node_tags(sn.id, sn.tags);
        }
        static std::string new_tag;
        const bool tag_submit = ImGui::InputText("add tag", &new_tag,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool tag_click = ImGui::SmallButton("+##tag");
        if (tag_submit || tag_click) {
            if (!new_tag.empty()) {
                // Dedup before adding so the chip row doesn't grow
                // visual duplicates that the DB would silently dedup.
                if (std::find(sn.tags.begin(), sn.tags.end(), new_tag) == sn.tags.end()) {
                    sn.tags.push_back(new_tag);
                    db->update_node_tags(sn.id, sn.tags);
                }
                new_tag.clear();
            }
        }

        // Timestamps — read-only display. 0 means "unknown" (legacy row
        // from before the schema migration).
        if (sn.first_seen > 0.0) {
            const std::time_t t = static_cast<std::time_t>(sn.first_seen);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            ImGui::Text("first seen   %s", buf);
        } else {
            ImGui::TextDisabled("first seen   (unknown)");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("set once when the node is created:\n"
                              "  - fresh DB initial node generation\n"
                              "  - click-to-pin promotion of a phantom\n"
                              "never updated after that.");
        }
        if (sn.last_touched > 0.0) {
            const std::time_t t = static_cast<std::time_t>(sn.last_touched);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            ImGui::Text("last touched %s", buf);
        } else {
            ImGui::TextDisabled("last touched (unknown)");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("bumped whenever the operator edits title or content.\n"
                              "tier changes, physics drift, and incoming edges do NOT bump.");
        }
        ImGui::Spacing();

        bool text_changed = false;
        text_changed |= ImGui::InputText("title", &sn.title);
        ImGui::TextDisabled("content (markdown)");
        text_changed |= ImGui::InputTextMultiline(
            "##content", &sn.content,
            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 12));
        if (text_changed) {
            // Persist the edit immediately so search picks it up on the
            // next keystroke; triggers keep the FTS index in sync.
            const double now_ts = zg::app::unix_now();
            sn.last_touched = now_ts;
            db->update_node_text(sn.id, sn.title, sn.content, now_ts);
            // The query may now have new hits.
            search_hits = db->search(search_query);

            // Log the edit. Record only lengths -- the operator's content
            // shouldn't leak into the telemetry log (privacy + bloat).
            // Co-located with the touch-edge logic below so pin-then-edit
            // analysis can join on (node_id, ts).
            nlohmann::json p = {
                {"node_id",     sn.id},
                {"title_len",   sn.title.size()},
                {"content_len", sn.content.size()},
                {"tier",        sn.tier},
            };
            db->log_event("node_edit", sn.id, p.dump());

            // Touch-edge: any operator edit on a non-self node should
            // create a record-of-attention from self -> that node.
            // certainty="hearsay" so the line renders dim and doesn't
            // visually compete with confirmed edges.
            if (self_idx < stored_nodes.size() &&
                self_idx != static_cast<std::size_t>(selected_node)) {
                const std::size_t sel = static_cast<std::size_t>(selected_node);
                bool already = false;
                for (const auto& e : edges) {
                    if ((e.source == self_idx && e.target == sel) ||
                        (e.source == sel && e.target == self_idx)) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    const zg::graph::Edge touch{
                        self_idx, sel, "touched", "saw-at", "hearsay"};
                    edges.push_back(touch);
                    physics->enqueue_edge(touch);
                }
            }

            // Wikilinks: parse [[title]] occurrences out of the content
            // and materialize them as edges from this node to whichever
            // existing node has a matching title. Skip duplicates so
            // re-saving doesn't fan out parallel edges. Edge persists
            // via the explicit save button or shutdown save.
            const auto refs = zg::graph::extract_wikilinks(sn.content);
            for (const std::string& title : refs) {
                if (title.empty()) continue;
                std::size_t target_idx = SIZE_MAX;
                for (std::size_t k = 0; k < stored_nodes.size(); ++k) {
                    if (stored_nodes[k].title == title) {
                        target_idx = k;
                        break;
                    }
                }
                if (target_idx == SIZE_MAX) continue;
                if (target_idx == static_cast<std::size_t>(selected_node)) continue;
                const std::size_t src_idx = static_cast<std::size_t>(selected_node);
                bool exists = false;
                for (const auto& e : edges) {
                    if ((e.source == src_idx && e.target == target_idx) ||
                        (e.source == target_idx && e.target == src_idx)) {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;
                const zg::graph::Edge link_edge{src_idx, target_idx};
                edges.push_back(link_edge);
                physics->enqueue_edge(link_edge);
            }
        }

        if (ImGui::Button("save to disk")) {
            for (std::size_t i = 0;
                 i < positions.size() && i < stored_nodes.size(); ++i) {
                stored_nodes[i].position = positions[i];
            }
            db->save_graph(stored_nodes, edges);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("deselect")) selected_node = -1;

        // Soft-delete: two-click arm/confirm pattern matches the project
        // delete affordance. Sets the tombstone flag, persists, logs a
        // node_delete event for the phase-2 pin-then-delete metric, then
        // deselects. The row stays in the DB so the events join
        // (phantom_pin -> node_delete) still finds the original pin row.
        // Self node can't be deleted -- it's structural.
        static bool delete_armed = false;
        const bool is_self = (static_cast<std::size_t>(selected_node) == self_idx);
        if (is_self) ImGui::BeginDisabled();
        if (!delete_armed) {
            if (ImGui::SmallButton("delete node...")) delete_armed = true;
        } else {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1.0f), "click again to confirm");
            ImGui::SameLine();
            if (ImGui::SmallButton("yes, delete")) {
                sn.deleted = true;
                db->save_graph(stored_nodes, edges);
                {
                    nlohmann::json p = {
                        {"node_id", sn.id},
                        {"title_len",   sn.title.size()},
                        {"content_len", sn.content.size()},
                        {"tier",        sn.tier},
                    };
                    db->log_event("node_delete", sn.id, p.dump());
                }
                selected_node  = -1;
                delete_armed   = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) delete_armed = false;
        }
        if (is_self) ImGui::EndDisabled();

        // "Ask about selection" -- pipes the selected node + its spatial
        // neighbourhood to the LLM and lets the resulting phantom land
        // via the normal UDP listener. Disabled while a prior ask is
        // still in flight so we never double-fire the subprocess. The
        // status line below the button surfaces any error from the
        // background worker (Ollama down / subprocess failure / etc).
        ImGui::Spacing();
        const auto ask_snap = s.ask.snapshot();
        const bool thinking = ask_snap.state == zg::app::LlmAsk::State::Thinking;
        if (thinking) ImGui::BeginDisabled();
        if (ImGui::Button("ask about selection")) {
            const std::string db_path =
                (std::filesystem::path("projects") /
                 (s.current_project + ".db")).string();
            s.ask.start(db_path, sn.id);
        }
        if (thinking) ImGui::EndDisabled();
        switch (ask_snap.state) {
            case zg::app::LlmAsk::State::Idle:
                break;
            case zg::app::LlmAsk::State::Thinking:
                ImGui::TextDisabled("%s", ask_snap.msg.c_str());
                break;
            case zg::app::LlmAsk::State::Done:
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                   "%s", ask_snap.msg.c_str());
                break;
            case zg::app::LlmAsk::State::ErrNoOllama:
            case zg::app::LlmAsk::State::ErrSubprocess:
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s", ask_snap.msg.c_str());
                break;
        }

        // Incident edges — list every edge touching the selected node
        // with editable label / kind / certainty. Updates persist
        // immediately via update_edge; no save-button needed.
        ImGui::Spacing();
        ImGui::Separator();
        const std::size_t sel = static_cast<std::size_t>(selected_node);
        int incident_count = 0;
        for (const auto& e : edges) {
            if (e.source == sel || e.target == sel) ++incident_count;
        }
        ImGui::Text("edges (%d incident)", incident_count);

        // Edge editor — flat layout, no CollapsingHeader.  Each row is
        // its own ImGui::PushID scope so the label/kind/certainty
        // widgets get unique IDs per edge.  A persistent edit_counter
        // ticks every time a change registers; if you edit and the
        // counter doesn't move, the change isn't being detected.
        static const char* kKindLabels[]  = {"(none)", "knows", "owns", "saw-at", "shell-of", "suspected"};
        static const char* kKindValues[]  = {"",       "knows", "owns", "saw-at", "shell-of", "suspected"};
        static const char* kCertainties[] = {"confirmed", "suspected", "hearsay", "phantom"};
        static int edge_edit_counter = 0;
        ImGui::TextDisabled("edge edits registered: %d", edge_edit_counter);
        ImGui::Spacing();

        for (std::size_t i = 0; i < edges.size(); ++i) {
            auto& e = edges[i];
            if (e.source != sel && e.target != sel) continue;
            const std::size_t other = (e.source == sel) ? e.target : e.source;
            const char* other_title = (other < stored_nodes.size() && !stored_nodes[other].title.empty())
                                      ? stored_nodes[other].title.c_str()
                                      : "(untitled)";
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            ImGui::Text("-> %zu  %s", other, other_title);

            bool changed = false;
            if (ImGui::InputText("label", &e.label))    changed = true;

            int kind_idx = 0;
            for (int k = 0; k < 6; ++k) if (e.kind == kKindValues[k]) { kind_idx = k; break; }
            if (ImGui::Combo("kind", &kind_idx, kKindLabels, 6)) {
                e.kind = kKindValues[kind_idx];
                changed = true;
            }

            int c_idx = 0;
            for (int k = 0; k < 4; ++k) if (e.certainty == kCertainties[k]) { c_idx = k; break; }
            if (ImGui::Combo("certainty", &c_idx, kCertainties, 4)) {
                e.certainty = kCertainties[c_idx];
                changed = true;
            }

            if (changed) {
                ++edge_edit_counter;
                db->update_edge(e.source, e.target, e.label, e.kind, e.certainty);
            }
            ImGui::PopID();
        }
    } else {
        ImGui::TextDisabled("selected: (none)");
        ImGui::TextDisabled("(left-click a node)");
    }
}

}  // namespace zg::ui
