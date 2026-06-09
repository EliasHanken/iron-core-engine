#include "editor/SceneInspector.h"

#include "editor/ReflectionInspector.h"
#include "reflection/FieldDesc.h"
#include "scene/SceneFormat.h"
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"

#include <imgui.h>

#include <span>
#include <string>

namespace iron {

bool SceneInspector::draw(const Reflection& reflection,
                          const ComponentRegistry& registry,
                          SceneEntity* entity,
                          GizmoSpace& space,
                          EffectKind& effectKind) {
    bool changed = false;
    // Submit the window EVERY frame, selection or not. Begin-ing it only when an
    // entity is selected makes it newly-appear on selection, and ImGui focuses a
    // freshly-submitted window — yanking the user off whatever panel they were in.
    ImGui::Begin("Inspector");
    if (!entity) {
        ImGui::TextDisabled("(no entity selected)");
        ImGui::End();
        return false;
    }
    SceneEntity& e = *entity;

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Gizmo space toggle (mirrors the X key). Scale handles are always local
    // regardless of this setting. Editor-tool state — not entity data, stays
    // hand-rolled.
    if (ImGui::CollapsingHeader("Gizmo Space", ImGuiTreeNodeFlags_DefaultOpen)) {
        int spaceInt = (space == GizmoSpace::Local) ? 1 : 0;
        bool spaceChanged = false;
        spaceChanged |= ImGui::RadioButton("World", &spaceInt, 0);
        ImGui::SameLine();
        spaceChanged |= ImGui::RadioButton("Local", &spaceInt, 1);
        if (spaceChanged)
            space = (spaceInt == 1) ? GizmoSpace::Local : GizmoSpace::World;
    }

    // Selection effect picker — editor tool state, not entity data.
    if (ImGui::CollapsingHeader("Selection Effect", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* kinds[] = {"None", "Outline", "Glowing Outline", "X-Ray"};
        int ki = static_cast<int>(effectKind);
        if (ImGui::Combo("Effect", &ki, kinds, 4))
            effectKind = static_cast<EffectKind>(ki);
    }

    // Entity body — purely reflection-driven.
    auto coreSection = [&](const char* label, std::span<const FieldDesc> fields, void* obj) {
        if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            changed |= renderComponentByPtr(reflection, std::string_view{}, fields, obj);
            ImGui::Unindent();
        }
    };
    coreSection("Transform", reflection.fieldsOf<Transform>(),   &e.transform);
    coreSection("Mesh",      reflection.fieldsOf<MeshRef>(),     &e.mesh);
    coreSection("Material",  reflection.fieldsOf<MaterialDef>(), &e.material);

    // Components on this entity (registry-driven; reflected editing + remove).
    for (auto& box : e.components.all()) {                 // non-const all() → mutable data()
        const ComponentRegistry::Entry* entry = registry.byTypeId(box->typeId());
        if (!entry) continue;
        ImGui::PushID(static_cast<int>(entry->typeId));
        const std::string header(entry->name);
        const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (open) {
            ImGui::Indent();
            changed |= renderComponentByPtr(reflection, std::string_view{}, entry->fields, box->data());
            if (ImGui::SmallButton("Remove")) {
                e.components.removeTypeId(entry->typeId);
                changed = true;
                ImGui::Unindent();
                ImGui::PopID();
                break;   // container mutated mid-iteration — stop this frame
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    // "Add Component" combo lists only the registered types this entity lacks.
    if (ImGui::BeginCombo("Add Component", "Add Component ...")) {
        for (std::uint32_t id : registry.order()) {
            if (e.components.hasTypeId(id)) continue;
            const ComponentRegistry::Entry* entry = registry.byTypeId(id);
            if (entry && ImGui::Selectable(std::string(entry->name).c_str())) {
                e.components.addBox(entry->factory());
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
