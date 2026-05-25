#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

#include <cstdint>
#include <optional>
#include <span>

namespace iron {

// A projectile in flight. Position + velocity in world space; no gravity,
// no player collision (caller handles lag-comp + splash). `alive` is
// flipped to false by tickProjectile on world hit or by the caller on
// lifetime expiry.
struct Projectile {
    std::uint32_t id = 0;
    std::uint32_t ownerPeerId = 0;
    Vec3 position{};
    Vec3 velocity{};
    double spawnTimeSec = 0.0;
    bool alive = true;
};

struct ProjectileHit {
    Vec3 point;
    Vec3 normal;
};

// Advance `p` by `dt` against a list of static world AABBs. If the swept
// segment hits a box (treated as a ray from current position with length
// |velocity|*dt), updates `p.position` to the hit point, sets
// `p.alive = false`, and returns the hit. Otherwise advances `p.position`
// by `velocity * dt` and returns nullopt.
std::optional<ProjectileHit> tickProjectile(
    Projectile& p, float dt, std::span<const Aabb> worldBoxes);

}  // namespace iron
