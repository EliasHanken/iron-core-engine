#include "editor/SceneInspector.h"

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneInspector::draw(SceneEntity& e) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

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
