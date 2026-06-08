#pragma once

#include "Messages.h"
#include "Projectile.h"

#include "common/WeaponCooldown.h"
#include "math/Aabb.h"
#include "math/Vec.h"

#include <cstdint>
#include <optional>
#include <span>

namespace iron::netshooter {

struct RocketLauncher {
    static constexpr float kCooldownSec    = 1.1f;     // ~0.9 shots/sec
    static constexpr float kMuzzleSpeed    = 30.0f;
    static constexpr float kMaxLifetimeSec = 5.0f;
    static constexpr int   kCenterDamage   = 80;
    static constexpr float kSplashRadius   = 4.0f;
    WeaponCooldown cooldown{kCooldownSec};
};

std::optional<FireRocketMsg> tryFireRocketClient(
    RocketLauncher& w, double nowSec,
    const Vec3& muzzle, const Vec3& aimDir, double viewTimeSec);

// CLIENT: visual-only ghost tick — linearly extrapolates the rocket's
// position from its last-known velocity. The host owns authoritative
// trajectory + collision via Jolt (see worldShared in main.cpp), and
// sends DespawnProjectileMsg on impact/expiry.
void tickRocketClient(Projectile& ghost, float dt,
                      std::span<const Aabb> worldBoxes);

}  // namespace iron::netshooter
