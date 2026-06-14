#include "scene/Prefab.h"

#include "scene/SceneHierarchy.h"   // collectSubtree

#include <unordered_map>

namespace iron {

Prefab extractPrefab(const SceneFile& scene, int root) {
    Prefab out;
    const std::vector<int> sub = collectSubtree(scene, root);   // root first, or empty
    if (sub.empty()) return out;

    // old scene index -> prefab-local index.
    std::unordered_map<int, int> remap;
    remap.reserve(sub.size());
    for (std::size_t k = 0; k < sub.size(); ++k) remap[sub[k]] = static_cast<int>(k);

    out.entities.reserve(sub.size());
    for (std::size_t k = 0; k < sub.size(); ++k) {
        SceneEntity e = scene.entities[sub[k]];   // value copy (transform + components)
        if (k == 0) {
            // collectSubtree guarantees sub[0] == root.
            e.parentIndex = -1;                   // root of the prefab
        } else {
            // A descendant's parent is always within the subtree (collectSubtree
            // gathered everything under root), so remap contains it.
            e.parentIndex = remap[e.parentIndex];
        }
        out.entities.push_back(std::move(e));
    }
    return out;
}

int instantiatePrefab(SceneFile& scene, const Prefab& prefab,
                      const Transform& placement,
                      const std::function<std::string(const std::string&)>& uniquify) {
    if (prefab.entities.empty()) return -1;
    const int base = static_cast<int>(scene.entities.size());

    for (std::size_t k = 0; k < prefab.entities.size(); ++k) {
        SceneEntity e = prefab.entities[k];        // value copy
        e.name = uniquify(e.name);
        if (k == 0) {
            e.parentIndex   = -1;                  // new scene root
            e.transform     = placement;           // caller chooses where it lands
        } else {
            // prefab-local parent index -> scene index (always >= 0 here).
            e.parentIndex = base + e.parentIndex;
        }
        scene.entities.push_back(std::move(e));
    }
    return base;
}

}  // namespace iron
