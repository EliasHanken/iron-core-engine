#pragma once

#include "math/Vec.h"

namespace iron {

// A mass point for Verlet integration. It stores no explicit velocity —
// velocity is implicit in (position - previousPosition). A pinned point is an
// anchor: integration never moves it.
struct VerletPoint {
    Vec3 position;
    Vec3 previousPosition;
    bool pinned = false;
};

// Advance one point by a single Verlet step under a constant acceleration.
// Pinned points are left untouched.
//
//   velocity   = position - previousPosition   (implicit)
//   next       = position + velocity + acceleration * dt*dt
//
inline void integrate(VerletPoint& p, Vec3 acceleration, float dt) {
    if (p.pinned) {
        return;
    }
    const Vec3 velocity = p.position - p.previousPosition;
    const Vec3 next = p.position + velocity + acceleration * (dt * dt);
    p.previousPosition = p.position;
    p.position = next;
}

} // namespace iron
