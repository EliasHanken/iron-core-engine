#pragma once

#include "editor/Gizmo.h"      // for GizmoSpace
#include "render/PostEffect.h" // for EffectKind

namespace iron {

struct SceneEntity;

// Details panel for a single entity: transform (position, euler rotation,
// scale) and material scalars (emissive, uvScale, reflectivity). Mesh info is
// shown read-only. Mutates the entity in place. Also hosts the gizmo World/Local
// space toggle (mirrors the `X` key), which it reads from and writes to `space`.
// Also hosts the selection post-process effect picker, which it reads from and
// writes to `effectKind` (editor tool state — not a scene-dirty field).
class SceneInspector {
public:
    // Returns true if any entity field changed this frame.
    bool draw(SceneEntity& entity, GizmoSpace& space, EffectKind& effectKind);
};

}  // namespace iron
