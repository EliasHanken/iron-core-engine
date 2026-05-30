#pragma once

#include "editor/Gizmo.h"  // for GizmoSpace

namespace iron {

struct SceneEntity;

// Details panel for a single entity: transform (position, euler rotation,
// scale) and material scalars (emissive, uvScale, reflectivity). Mesh info is
// shown read-only. Mutates the entity in place. Also hosts the gizmo World/Local
// space toggle (mirrors the `X` key), which it reads from and writes to `space`.
class SceneInspector {
public:
    // Returns true if any entity field changed this frame.
    bool draw(SceneEntity& entity, GizmoSpace& space);
};

}  // namespace iron
