#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

namespace iron {

// True if a sphere of `radius` centered at `center` overlaps `box`.
// Uses the closest-point-on-AABB distance check; faster than naive
// per-axis tests because it short-circuits on the squared distance.
bool sphereOverlapAabb(const Vec3& center, float radius, const Aabb& box);

}  // namespace iron
