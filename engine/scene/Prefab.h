#pragma once

#include "scene/SceneFormat.h"
#include "world/Transform.h"

#include <functional>
#include <string>
#include <vector>

namespace iron {

// A self-contained, reusable subtree. entities[0] is the root; every
// parentIndex is PREFAB-LOCAL (root = -1, descendants index into this vector).
// Carries each entity's transform / mesh / material / components, so geometry,
// materials, nested hierarchy, and gameplay components all travel with it.
struct Prefab {
    int                      version = 1;
    std::vector<SceneEntity> entities;
};

// Extract scene entity `root` and its whole subtree into a Prefab. The root
// becomes index 0 with parentIndex -1; descendant parentIndex values are
// re-based to prefab-local space. The root's own transform is copied verbatim
// (placement is applied at instantiate time, not here). Returns an empty Prefab
// if `root` is out of range.
Prefab extractPrefab(const SceneFile& scene, int root);

// Append `prefab`'s entities to scene.entities. Internal parent links are
// re-indexed into scene space; names are uniquified via `uniquify`; the new
// root's transform is REPLACED by `placement` (descendants keep their
// prefab-local transforms, preserving the subtree's internal layout). Returns
// the new root's index in scene.entities, or -1 if the prefab is empty. Existing
// indices are unchanged (copies are appended), so the host only spawns World
// entities for the appended range — identical to duplicateSubtree.
int instantiatePrefab(SceneFile& scene, const Prefab& prefab,
                      const Transform& placement,
                      const std::function<std::string(const std::string&)>& uniquify);

}  // namespace iron
