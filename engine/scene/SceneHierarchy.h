#pragma once

#include "math/Mat4.h"
#include "scene/SceneFormat.h"

#include <functional>
#include <string>
#include <vector>

namespace iron {

// Depth cap shared by every chain walk; doubles as a cycle guard so a
// hand-corrupted parentIndex never hangs.
inline constexpr int kMaxHierarchyDepth = 256;

// World matrix of entity `index`: its local matrix pre-multiplied by every
// ancestor's, walking up parentIndex. Returns identity for an out-of-range
// index. Depth-capped.
Mat4 worldMatrixOf(const SceneFile& scene, int index);

// True if `maybeDescendant` is `ancestor` or sits anywhere below it.
bool isDescendant(const SceneFile& scene, int ancestor, int maybeDescendant);

// `root` followed by all its descendants (any order below root). Empty if root
// is out of range.
std::vector<int> collectSubtree(const SceneFile& scene, int root);

// Reparent `child` under `newParent` (-1 = make root), preserving world pose:
// the child's local transform is recomputed as inverse(parentWorld) * childWorld.
// Returns false (no change) for an invalid child, an out-of-range newParent,
// self-parenting, or a newParent that is a descendant of child (would cycle).
// LOSSY under non-uniform parent scale combined with child rotation (shear has
// no TRS representation) — the standard engine caveat.
bool reparentKeepWorld(SceneFile& scene, int child, int newParent);

// Remove `root` and its whole subtree. Returns an old->new index map sized to
// the ORIGINAL entity count: removed entries are -1, survivors map to their new
// index. Surviving parentIndex links are remapped. The host uses the map to fix
// sceneIndexToEntity / resolved / selectedIndex in one place.
std::vector<int> deleteSubtree(SceneFile& scene, int root);

// Deep-copy `root`'s subtree, appending the copies. Internal parent links are
// preserved; the new root attaches to the source root's parent. Each new entity
// is renamed via `uniquify(name)`. Returns the new root's index (-1 if root is
// out of range). Existing indices are unchanged (copies are appended), so the
// host only needs to spawn World entities for the appended range.
int duplicateSubtree(SceneFile& scene, int root,
                     const std::function<std::string(const std::string&)>& uniquify);

}  // namespace iron
