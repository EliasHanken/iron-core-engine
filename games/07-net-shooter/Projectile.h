#pragma once

#include "math/Vec.h"
#include <cstdint>

namespace iron::netshooter {

// Minimal POD for tracking a rocket's state. Used by:
//  - client-side `ghostRockets` (rendered as a sphere flying through
//    the arena based on last-known position + velocity; advanced via
//    linear extrapolation each frame).
//  - the `SpawnProjectileMsg` payload (initial pos+vel from host).
// Host no longer uses this struct — its rockets live in `worldShared`
// as Jolt dynamic spheres (see `HostRocket` in main.cpp).
struct Projectile {
    std::uint32_t id = 0;
    std::uint32_t ownerPeerId = 0;
    Vec3 position{};
    Vec3 velocity{};
    double spawnTimeSec = 0.0;
    bool alive = true;
};

}  // namespace iron::netshooter
