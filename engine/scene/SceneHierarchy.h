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

}  // namespace iron
