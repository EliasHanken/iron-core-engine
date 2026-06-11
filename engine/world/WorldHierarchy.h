#pragma once

#include "math/Mat4.h"
#include "world/Entity.h"

#include <cstdint>
#include <unordered_map>

namespace iron {

class World;

// Compose `e`'s model matrix through its Parent chain (depth-capped as a cycle
// guard). An entity with no Parent component / kEntityNone parent is treated as
// a root (its local matrix). Returns identity if `e` has no Transform.
Mat4 worldMatrix(const World& world, EntityId e);

// Memoized variant for per-frame iteration over many entities: caches by
// entity index (not full generation) so a shared ancestor is composed once
// per frame. The memo MUST be declared locally and discarded before any
// World::create/destroy call that could recycle entity indices; a stale
// memo across entity recycling returns incorrect matrices silently.
Mat4 worldMatrix(const World& world, EntityId e,
                 std::unordered_map<std::uint32_t, Mat4>& memo);

}  // namespace iron
