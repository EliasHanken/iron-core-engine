#include "physics/Rope.h"

#include <cstddef>

namespace iron {

Rope::Rope(Vec3 endA, Vec3 endB, int segments, float ropeLength) {
    if (segments < 1) {
        segments = 1;
    }
    const int pointCount = segments + 1;

    // Lay the points out in a straight line between the endpoints; gravity
    // pulls the slack into a hanging curve over the first few updates.
    points_.resize(static_cast<std::size_t>(pointCount));
    for (int i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const Vec3 p = endA + (endB - endA) * t;
        points_[static_cast<std::size_t>(i)].position = p;
        points_[static_cast<std::size_t>(i)].previousPosition = p;
    }
    points_.front().pinned = true;
    points_.back().pinned = true;

    // restLength is derived from the rope's natural length, NOT the endpoint
    // distance — that is what lets the rope carry slack.
    const float restLength = ropeLength / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        constraints_.push_back(DistanceConstraint{i, i + 1, restLength});
    }
}

// These move only `position`. `previousPosition` is left stale, which is
// harmless while the endpoints stay pinned (integration skips pinned points).
// TODO(M4): when an endpoint can be unpinned (cut/released rope), decide
// whether the freed end should keep its drag momentum or start at rest, and
// set previousPosition accordingly here.
void Rope::setEndpointA(Vec3 position) {
    points_.front().position = position;
}

void Rope::setEndpointB(Vec3 position) {
    points_.back().position = position;
}

void Rope::update(float dt) {
    for (VerletPoint& p : points_) {
        integrate(p, gravity_, dt);
    }
    // More iterations make the rope stiffer and less stretchy.
    for (int iter = 0; iter < iterations_; ++iter) {
        for (const DistanceConstraint& c : constraints_) {
            satisfy(c, points_);
        }
    }
}

} // namespace iron
