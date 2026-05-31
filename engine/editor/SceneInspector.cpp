#include "editor/SceneInspector.h"

#include "editor/ReflectionInspector.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneInspector::draw(const Reflection& reflection,
                          SceneEntity& e,
                          GizmoSpace& space,
                          EffectKind& effectKind) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Gizmo space toggle (mirrors the X key). Scale handles are always local
    // regardless of this setting. Editor-tool state — not entity data, stays
    // hand-rolled.
    ImGui::SeparatorText("Gizmo Space");
    int spaceInt = (space == GizmoSpace::Local) ? 1 : 0;
    bool spaceChanged = false;
    spaceChanged |= ImGui::RadioButton("World", &spaceInt, 0);
    ImGui::SameLine();
    spaceChanged |= ImGui::RadioButton("Local", &spaceInt, 1);
    if (spaceChanged)
        space = (spaceInt == 1) ? GizmoSpace::Local : GizmoSpace::World;

    // Selection effect picker — editor tool state, not entity data.
    ImGui::SeparatorText("Selection Effect");
    const char* kinds[] = {"None", "Outline", "Glowing Outline", "X-Ray"};
    int ki = static_cast<int>(effectKind);
    if (ImGui::Combo("Effect", &ki, kinds, 4))
        effectKind = static_cast<EffectKind>(ki);

    // Entity body — purely reflection-driven.
    changed |= renderComponent(reflection, e.transform);
    changed |= renderComponent(reflection, e.mesh);
    changed |= renderComponent(reflection, e.material);

    ImGui::End();
    return changed;
}

}  // namespace iron
