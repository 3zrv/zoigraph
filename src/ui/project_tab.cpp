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
                        const std::function<void(const std::string&)>& open_project) {
    auto& current_project = s.current_project;
    auto& stored_nodes    = s.stored_nodes;
    auto& edges           = s.edges;
    auto& cluster_ids     = s.cluster_ids;

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

        // Delete-current arming. First click sets the arm flag; second
        // confirms. Disables itself if there's only one project left
        // (always need at least one to be active).
        static bool delete_armed = false;
        const bool only_one = names.size() <= 1;
        if (only_one) ImGui::BeginDisabled();
        if (!delete_armed) {
            if (ImGui::SmallButton("delete current...")) delete_armed = true;
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
                delete_armed = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) delete_armed = false;
        }
        if (only_one) ImGui::EndDisabled();
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
}

}  // namespace zg::ui
