#include "editor/SceneOutliner.h"

#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    bool saveClicked = false;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) saveClicked = true;
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();
        ImGui::PushID(i);  // unique id even when names collide
        if (ImGui::Selectable(label, i == selectedIndex)) selectedIndex = i;
        ImGui::PopID();
    }

    ImGui::End();
    return saveClicked;
}

}  // namespace iron
