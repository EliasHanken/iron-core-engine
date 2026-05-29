#include "editor/SceneOutliner.h"

#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

SceneOutliner::Result SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    Result result;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) result.saveClicked = true;
    ImGui::Separator();

    // Entity list.
    for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();
        ImGui::PushID(i);
        if (ImGui::Selectable(label, i == selectedIndex)) selectedIndex = i;
        ImGui::PopID();
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
