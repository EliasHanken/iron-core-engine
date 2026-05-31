#pragma once

#include "editor/Gizmo.h"      // for GizmoSpace
#include "render/PostEffect.h" // for EffectKind

namespace iron {

struct SceneEntity;
class  Reflection;

// Details panel for a single entity. Renders transform / mesh / material via
// iron::Reflection-driven dispatch. Editor-tool widgets (gizmo space, effect
// picker) stay hand-rolled — they are not entity data.
class SceneInspector {
public:
    // Returns true if any entity field changed this frame.
    bool draw(const Reflection& reflection,
              SceneEntity& entity,
              GizmoSpace& space,
              EffectKind& effectKind);
};

}  // namespace iron
