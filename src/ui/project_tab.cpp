#include "ui/project_tab.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "graph/cluster.h"
#include "persistence/project_store.h"

namespace zg::ui {

void render_project_tab(zg::app::Session& s,
                        const std::filesystem::path& projects_dir,
                        const std::function<void(const std::string&)>& open_project,
                        bool& dim_filtered,
                        bool& show_grid,
                        bool& post_process) {
    auto& current_project = s.current_project;
    auto& stored_nodes    = s.stored_nodes;
    auto& edges           = s.edges;
    auto& cluster_ids     = s.cluster_ids;
    auto& tag_filter      = s.tag_filter;

    ImGui::Text("active: %s", current_project.c_str());
    {
        const auto names = zg::persistence::list_projects(projects_dir);
        int active_idx = -1;
        std::vector<const char*> ptrs;
        ptrs.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            ptrs.push_back(names[i].c_str());
            if (names[i] == current_project) active_idx = static_cast<int>(i);
        }
        if (!ptrs.empty()) {
            if (ImGui::Combo("switch", &active_idx, ptrs.data(),
                             static_cast<int>(ptrs.size())) &&
                active_idx >= 0 &&
                names[static_cast<std::size_t>(active_idx)] != current_project) {
                open_project(names[static_cast<std::size_t>(active_idx)]);
            }
        }

        static std::string new_name;
        static std::string create_msg;
        const bool submitted = ImGui::InputText("new", &new_name,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) create_msg.clear();
        ImGui::SameLine();
        const bool clicked = ImGui::SmallButton("create");
        if (submitted || clicked) {
            if (new_name.empty()) {
                create_msg = "name is empty";
            } else if (!zg::persistence::is_valid_project_name(new_name)) {
                create_msg = "name must be 1-64 chars: [A-Za-z0-9_-]";
            } else if (std::filesystem::exists(
                           zg::persistence::project_path(projects_dir, new_name))) {
                create_msg = "project already exists";
            } else {
                const std::string to_open = new_name;
                new_name.clear();
                create_msg.clear();
                open_project(to_open);
            }
        }
        if (!create_msg.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", create_msg.c_str());
        }

        // Delete-current arming. First click sets the arm state; second
        // confirms. Disables itself if there's only one project left
        // (always need at least one to be active). Armed state is keyed
        // to the project it was armed on: switching projects mid-confirm
        // must disarm, otherwise the confirm deletes the NEW project.
        static std::string delete_armed_project;
        const bool delete_armed = (delete_armed_project == current_project)
                                  && !current_project.empty();
        const bool only_one = names.size() <= 1;
        if (only_one) ImGui::BeginDisabled();
        if (!delete_armed) {
            if (ImGui::SmallButton("delete current...")) {
                delete_armed_project = current_project;
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1.0f), "click again to confirm");
            ImGui::SameLine();
            if (ImGui::SmallButton("yes, delete")) {
                const std::string victim = current_project;
                std::string fallback;
                for (const auto& n : names) {
                    if (n != victim) { fallback = n; break; }
                }
                if (!fallback.empty()) {
                    open_project(fallback);
                    zg::persistence::delete_project(projects_dir, victim);
                }
                delete_armed_project.clear();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) delete_armed_project.clear();
        }
        if (only_one) ImGui::EndDisabled();
    }

    ImGui::Separator();

    // ---- tag filter -------------------------------------------------
    // Collect the unique set of tags across all nodes and expose them as
    // a combo. Selecting one highlights matching nodes in the 3D view;
    // "(all)" clears the filter.
    {
        std::vector<std::string> unique_tags = {"(all)"};
        std::set<std::string>    seen;
        for (const auto& sn : stored_nodes) {
            for (const auto& t : sn.tags) {
                if (seen.insert(t).second) unique_tags.push_back(t);
            }
        }
        int filter_idx = 0;
        for (std::size_t k = 1; k < unique_tags.size(); ++k) {
            if (unique_tags[k] == tag_filter) {
                filter_idx = static_cast<int>(k);
                break;
            }
        }
        std::vector<const char*> filter_ptrs;
        filter_ptrs.reserve(unique_tags.size());
        for (const auto& tag : unique_tags) filter_ptrs.push_back(tag.c_str());
        if (ImGui::Combo("filter by tag", &filter_idx,
                         filter_ptrs.data(),
                         static_cast<int>(filter_ptrs.size()))) {
            tag_filter = (filter_idx == 0) ? "" : unique_tags[static_cast<std::size_t>(filter_idx)];
        }
        // Dimming only matters when a filter is active; gray the checkbox
        // out otherwise so the operator knows the toggle is dormant.
        if (tag_filter.empty()) ImGui::BeginDisabled();
        ImGui::Checkbox("dim non-matching nodes", &dim_filtered);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("when a tag filter is active, render non-matching\n"
                              "nodes in a dimmer red so the matching ones pop.");
        }
        if (tag_filter.empty()) ImGui::EndDisabled();
    }

    ImGui::Separator();

    // ---- clustering -------------------------------------------------
    // cluster_ids is the source of truth, populated on demand, displayed
    // by the cluster halo around each node in the 3D view.
    ImGui::TextDisabled("clustering");
    if (ImGui::Button("auto-cluster")) {
        cluster_ids = zg::graph::label_propagation(
            stored_nodes.size(), edges, /*max_iters=*/100);
    }
    ImGui::SameLine();
    if (ImGui::Button("clear clusters")) {
        cluster_ids.clear();
    }
    if (!cluster_ids.empty()) {
        std::set<std::size_t> unique_clusters(cluster_ids.begin(), cluster_ids.end());
        ImGui::TextDisabled("%zu clusters across %zu nodes",
                            unique_clusters.size(), cluster_ids.size());
    }

    ImGui::Separator();

    // ---- view flags -------------------------------------------------
    ImGui::Checkbox("show grid", &show_grid);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("draw the reference ground plane (40x40 raylib grid).\n"
                          "purely visual; doesn't affect physics or selection.");
    }
    ImGui::Checkbox("CRT post-process", &post_process);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("composite the 3D scene through the CRT shader:\n"
                          "chromatic aberration, scrolling scanlines, vignette.\n"
                          "turn off if your GPU struggles or you want crisp pixels.");
    }
    if (s.physics) {
        bool bh = s.physics->use_barnes_hut();
        if (ImGui::Checkbox("Barnes-Hut physics", &bh)) {
            s.physics->set_use_barnes_hut(bh);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("approximate repulsion with an octree (O(N log N))\n"
                              "instead of all-pairs (O(N^2)). Cheaper at large N,\n"
                              "slightly less accurate. Spring forces are unchanged.");
        }
    }
}

}  // namespace zg::ui
