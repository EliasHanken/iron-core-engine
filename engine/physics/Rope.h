#pragma once

#include "math/Vec.h"
#include "physics/DistanceConstraint.h"
#include "physics/VerletPoint.h"

#include <vector>

namespace iron {

// A rope simulated as a chain of VerletPoints linked by DistanceConstraints.
// Both ends are pinned anchors — reposition them each frame with
// setEndpointA / setEndpointB. The middle hangs and swings under gravity.
class Rope {
public:
    // Builds `segments` segments (segments + 1 points) evenly spaced along the
    // line from `endA` to `endB`. `ropeLength` is the rope's natural length:
    // make it larger than the distance between the endpoints to give the rope
    // slack so it visibly dangles. Both endpoints start pinned.
    Rope(Vec3 endA, Vec3 endB, int segments, float ropeLength);

    void setEndpointA(Vec3 position);  // moves the first point
    void setEndpointB(Vec3 position);  // moves the last point

    // Advance the simulation one fixed step: integrate every point under
    // gravity, then satisfy the distance constraints `iterations_` times.
    void update(float dt);

    const std::vector<VerletPoint>& points() const { return points_; }

private:
    std::vector<VerletPoint> points_;
    std::vector<DistanceConstraint> constraints_;
    Vec3 gravity_{0.0f, -9.8f, 0.0f};
    int iterations_ = 16;
};

} // namespace iron
