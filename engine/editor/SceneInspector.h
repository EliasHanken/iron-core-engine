#pragma once

namespace iron {

struct SceneEntity;

// Details panel for a single entity: transform (position, euler rotation,
// scale) and material scalars (emissive, uvScale, reflectivity). Mesh info is
// shown read-only. Mutates the entity in place.
class SceneInspector {
public:
    // Returns true if any field changed this frame.
    bool draw(SceneEntity& entity);
};

}  // namespace iron
