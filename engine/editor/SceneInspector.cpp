#include "editor/SceneInspector.h"

#include "audio/AudioEmitter.h"
#include "editor/ReflectionInspector.h"
#include "scene/SceneFormat.h"
#include "world/CollisionShape.h"

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

    // M42: optional components (collision / audio). Table-driven so the combo +
    // render loop stay generic; a future optional component is one row here +
    // one std::optional field on SceneEntity. (A fully generic world.add<T>
    // combo waits for the World-migration milestone.)
    struct OptionalComp {
        const char* label;
        bool (*present)(const SceneEntity&);
        void (*attach)(SceneEntity&);
        void (*remove)(SceneEntity&);
        bool (*render)(const Reflection&, SceneEntity&);
    };
    static const OptionalComp kOptional[] = {
        { "CollisionShape",
          [](const SceneEntity& s){ return s.collision.has_value(); },
          [](SceneEntity& s){ s.collision.emplace(); },
          [](SceneEntity& s){ s.collision.reset(); },
          [](const Reflection& r, SceneEntity& s){ return renderComponent(r, *s.collision); } },
        { "AudioEmitter",
          [](const SceneEntity& s){ return s.audio.has_value(); },
          [](SceneEntity& s){ s.audio.emplace(); },
          [](SceneEntity& s){ s.audio.reset(); },
          [](const Reflection& r, SceneEntity& s){ return renderComponent(r, *s.audio); } },
    };

    for (const OptionalComp& oc : kOptional) {
        if (!oc.present(e)) continue;
        changed |= oc.render(reflection, e);
        ImGui::PushID(oc.label);
        if (ImGui::SmallButton("Remove")) { oc.remove(e); changed = true; }
        ImGui::PopID();
    }

    // "Add Component" combo lists only the optionals this entity lacks.
    if (ImGui::BeginCombo("Add Component", "Add Component ...")) {
        for (const OptionalComp& oc : kOptional) {
            if (oc.present(e)) continue;
            if (ImGui::Selectable(oc.label)) { oc.attach(e); changed = true; }
        }
        ImGui::EndCombo();
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
