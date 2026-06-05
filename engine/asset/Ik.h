#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"

namespace iron {

// Result of a two-bone solve. Positions are world-space; deltas are world-space
// rotations to apply on top of the root/mid bones' current global rotations.
struct TwoBoneIKResult {
    Vec3 midPos;     // new world position of the mid joint (knee/elbow)
    Vec3 endPos;     // new world position of the end joint (~target if reachable)
    Quat rootDelta;  // world-space rotation delta for the root bone
    Quat midDelta;   // world-space rotation delta for the mid bone
};

// Shortest-arc rotation taking `from` onto `to` (inputs need not be unit).
// Identity for parallel inputs; a 180-degree rotation about an arbitrary
// perpendicular for anti-parallel inputs.
Quat rotationFromTo(Vec3 from, Vec3 to);

// Analytic two-bone IK. `root`/`mid`/`end` are the current world joint
// positions; `target` is where the end should reach; `pole` hints the bend
// direction (the mid joint bends toward the side of `pole`). The target is
// clamped to the reachable range so the chain never tears or NaNs.
TwoBoneIKResult solveTwoBoneIK(Vec3 root, Vec3 mid, Vec3 end,
                               Vec3 target, Vec3 pole);

}  // namespace iron
