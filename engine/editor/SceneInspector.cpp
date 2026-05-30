#include "editor/SceneInspector.h"

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneInspector::draw(SceneEntity& e, GizmoSpace& space, EffectKind& effectKind) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Gizmo space toggle (mirrors the X key). Scale handles are always local
    // regardless of this setting.
    ImGui::SeparatorText("Gizmo Space");
    int spaceInt = (space == GizmoSpace::Local) ? 1 : 0;
    bool spaceChanged = false;
    spaceChanged |= ImGui::RadioButton("World", &spaceInt, 0);
    ImGui::SameLine();
    spaceChanged |= ImGui::RadioButton("Local", &spaceInt, 1);
    if (spaceChanged)
        space = (spaceInt == 1) ? GizmoSpace::Local : GizmoSpace::World;

    // Selection effect picker (editor tool state — not a scene-dirty field).
    ImGui::SeparatorText("Selection Effect");
    const char* kinds[] = {"None", "Outline", "Glowing Outline", "X-Ray"};
    int ki = static_cast<int>(effectKind);
    if (ImGui::Combo("Effect", &ki, kinds, 4))
        effectKind = static_cast<EffectKind>(ki);

    ImGui::SeparatorText("Transform");
    changed |= ImGui::DragFloat3("Position", &e.position.x, 0.05f);

    Vec3 euler = quatToEuler(e.rotation);  // degrees
    if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
        e.rotation = eulerToQuat(euler);
        changed = true;
    }

    changed |= ImGui::DragFloat3("Scale", &e.scale.x, 0.05f);

    ImGui::SeparatorText("Material");
    changed |= ImGui::ColorEdit3("Emissive", &e.material.emissive.x);
    changed |= ImGui::DragFloat("UV Scale", &e.material.uvScale, 0.05f, 0.0f, 64.0f);
    changed |= ImGui::SliderFloat("Reflectivity", &e.material.reflectivity, 0.0f, 1.0f);

    ImGui::SeparatorText("Mesh (read-only)");
    if (e.mesh.primitive.has_value()) {
        ImGui::Text("primitive: %s",
                    e.mesh.primitive.value() == PrimitiveKind::Cube ? "cube" : "plane");
    } else {
        ImGui::Text("gltf: %s", e.mesh.gltfPath.c_str());
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
