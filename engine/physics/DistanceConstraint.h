#pragma once

#include "math/Vec.h"
#include "physics/VerletPoint.h"

#include <cstddef>
#include <vector>

namespace iron {

// Keeps two points — referenced by index into a point array — a fixed
// distance apart. This is what gives a rope its stiffness.
struct DistanceConstraint {
    int a = 0;
    int b = 0;
    float restLength = 0.0f;
};

// Nudge the two constrained points toward restLength. Each free point takes
// half the correction; if one point is pinned, the free point takes all of
// it and the pinned point stays put. If both are pinned, nothing moves.
inline void satisfy(const DistanceConstraint& c, std::vector<VerletPoint>& points) {
    VerletPoint& pa = points[static_cast<std::size_t>(c.a)];
    VerletPoint& pb = points[static_cast<std::size_t>(c.b)];

    const Vec3 delta = pb.position - pa.position;
    const float dist = length(delta);
    if (dist <= 1e-8f) {
        return;  // degenerate: the points coincide, no defined direction
    }

    // `correction` points from a toward b; positive when the points are too
    // far apart, negative when too close.
    const float scale = (dist - c.restLength) / dist;
    const Vec3 correction = delta * scale;

    if (pa.pinned && pb.pinned) {
        return;
    }
    if (pa.pinned) {
        pb.position = pb.position - correction;
    } else if (pb.pinned) {
        pa.position = pa.position + correction;
    } else {
        pa.position = pa.position + correction * 0.5f;
        pb.position = pb.position - correction * 0.5f;
    }
}

} // namespace iron
