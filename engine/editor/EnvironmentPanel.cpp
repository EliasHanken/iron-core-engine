#include "editor/EnvironmentPanel.h"

#include "render/Fog.h"
#include "render/Light.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool EnvironmentPanel::draw(SceneFile& scene) {
    bool changed = false;
    ImGui::Begin("Environment");

    changed |= ImGui::ColorEdit3("Clear Color", &scene.clearColor.x);

    ImGui::SeparatorText("Sun");
    changed |= ImGui::DragFloat3("Direction", &scene.sun.direction.x, 0.02f, -1.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Sun Color", &scene.sun.color.x);
    changed |= ImGui::SliderFloat("Ambient", &scene.sun.ambient, 0.0f, 1.0f);

    ImGui::SeparatorText("Fog");
    changed |= ImGui::ColorEdit3("Fog Color", &scene.fog.color.x);
    changed |= ImGui::DragFloat("Fog Density", &scene.fog.density, 0.001f, 0.0f, 1.0f);

    ImGui::SeparatorText("Point Lights");
    for (int i = 0; i < static_cast<int>(scene.pointLights.size()); ++i) {
        ImGui::PushID(i);
        PointLight& pl = scene.pointLights[i];
        ImGui::Text("Light %d", i);
        changed |= ImGui::DragFloat3("Position", &pl.position.x, 0.05f);
        changed |= ImGui::ColorEdit3("Color", &pl.color.x);
        changed |= ImGui::DragFloat("Intensity", &pl.intensity, 0.05f, 0.0f, 50.0f);
        changed |= ImGui::DragFloat("Range", &pl.range, 0.1f, 0.0f, 200.0f);
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
