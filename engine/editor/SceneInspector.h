#pragma once

#include "editor/Gizmo.h"      // for GizmoSpace
#include "render/PostEffect.h" // for EffectKind

namespace iron {

struct SceneEntity;
class  Reflection;
class  ComponentRegistry;

// Details panel for a single entity. Renders transform / mesh / material via
// iron::Reflection-driven dispatch. Editor-tool widgets (gizmo space, effect
// picker) stay hand-rolled — they are not entity data.
class SceneInspector {
public:
    // Returns true if any entity field changed this frame. `entity` may be null
    // (no selection) — the window is still submitted every frame (with a
    // placeholder) so it never newly-appears on selection and steals focus.
    bool draw(const Reflection& reflection,
              const ComponentRegistry& registry,
              SceneEntity* entity,
              GizmoSpace& space,
              EffectKind& effectKind);
};

}  // namespace iron
