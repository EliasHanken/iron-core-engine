#include "editor/SceneOutliner.h"

#include "scene/SceneFormat.h"
#include "world/HierarchyConfig.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace iron {

SceneOutliner::Result SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    Result result;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) result.saveClicked = true;
    ImGui::Separator();

    // Entity tree (M69): parentIndex-driven hierarchy with drag-drop reparent.
    const int n = static_cast<int>(scene.entities.size());

    // Build children adjacency from parentIndex.
    std::vector<std::vector<int>> children(n);
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int p = scene.entities[i].parentIndex;
        if (p >= 0 && p < n) children[p].push_back(i);
        else                 roots.push_back(i);
    }

    // Recursive node renderer. Returns via `result` on a drag-drop reparent.
    // `depth` guards against a corrupted in-memory parent cycle (the scene is
    // sanitized at load and reparent rejects cycles, but don't hang if it slips).
    std::function<void(int, int)> drawNode = [&](int i, int depth) {
        if (depth > kMaxHierarchyDepth) return;

        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_DefaultOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (children[i].empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (i == selectedIndex)  flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(i);
        const bool open = ImGui::TreeNodeEx(label, flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectedIndex = i;

        // Drag source: payload = this entity index.
        if (!ImGui::IsItemToggledOpen() && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("IRON_ENTITY", &i, sizeof(int));
            ImGui::TextUnformatted(label);
            ImGui::EndDragDropSource();
        }
        // Drop target: reparent the dragged entity under this one.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IRON_ENTITY")) {
                const int child = *static_cast<const int*>(pl->Data);
                if (child != i) {            // self-drop is a no-op; deeper checks host-side
                    result.action            = Result::Action::Reparent;
                    result.reparentChild     = child;
                    result.reparentNewParent = i;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (open) {
            for (int c : children[i]) drawNode(c, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    for (int r : roots) drawNode(r, 0);

    // Drop zone to unparent (reparent to root).
    ImGui::Separator();
    ImGui::Selectable("[ Drop here to unparent ]");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IRON_ENTITY")) {
            const int child = *static_cast<const int*>(pl->Data);
            result.action            = Result::Action::Reparent;
            result.reparentChild     = child;
            result.reparentNewParent = -1;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();

    // Add bar.
    if (ImGui::Button("+ Cube"))  result.action = Result::Action::AddCube;
    ImGui::SameLine();
    if (ImGui::Button("+ Plane")) result.action = Result::Action::AddPlane;

    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##gltfpath", gltfPathBuf_, sizeof(gltfPathBuf_));
    ImGui::SameLine();
    if (ImGui::Button("+ glTF") && gltfPathBuf_[0] != '\0') {
        result.action   = Result::Action::AddGltf;
        result.gltfPath = gltfPathBuf_;
    }

    const bool hasSelection = selectedIndex >= 0 &&
                              selectedIndex < static_cast<int>(scene.entities.size());
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("Duplicate")) result.action = Result::Action::Duplicate;
    ImGui::SameLine();
    if (ImGui::Button("Delete"))    result.action = Result::Action::Delete;
    ImGui::EndDisabled();

    ImGui::End();
    return result;
}

}  // namespace iron
