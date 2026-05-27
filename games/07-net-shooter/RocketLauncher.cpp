#include "RocketLauncher.h"

namespace iron::netshooter {

std::optional<FireRocketMsg> tryFireRocketClient(
    RocketLauncher& w, double nowSec,
    const Vec3& muzzle, const Vec3& aimDir, double viewTimeSec) {
    if (!w.cooldown.tryFire(nowSec)) return std::nullopt;
    return FireRocketMsg{muzzle.x, muzzle.y, muzzle.z,
                         aimDir.x, aimDir.y, aimDir.z,
                         viewTimeSec};
}

void tickRocketClient(Projectile& ghost, float dt,
                      std::span<const Aabb> /*worldBoxes*/) {
    if (!ghost.alive) return;
    // M20 — clients no longer simulate collision. Host owns the
    // authoritative trajectory; clients linearly extrapolate the
    // rocket until DespawnProjectileMsg arrives.
    ghost.position.x += ghost.velocity.x * dt;
    ghost.position.y += ghost.velocity.y * dt;
    ghost.position.z += ghost.velocity.z * dt;
}

}  // namespace iron::netshooter
