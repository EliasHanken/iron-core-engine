#include "HitscanRifle.h"

#include "math/Ray.h"

namespace iron::netshooter {

std::optional<FireHitscanMsg> tryFireHitscanClient(
    HitscanRifle& w, double nowSec,
    const Vec3& muzzle, const Vec3& aimDir, double viewTimeSec) {
    if (!w.cooldown.tryFire(nowSec)) return std::nullopt;
    return FireHitscanMsg{muzzle.x, muzzle.y, muzzle.z,
                          aimDir.x, aimDir.y, aimDir.z,
                          viewTimeSec};
}

HitscanResolution resolveHitscanHost(
    HitscanRifle& serverWeapon, double nowSec,
    std::uint32_t shooterPeerId, const FireHitscanMsg& msg,
    const LagCompensator& lagComp,
    std::span<const Aabb> worldBoxes,
    std::span<const std::uint32_t> alivePeers,
    const Vec3& playerHalfExtents) {
    HitscanResolution out;
    if (!serverWeapon.cooldown.tryFire(nowSec)) return out;

    const Vec3 origin{msg.ox, msg.oy, msg.oz};
    const Vec3 dir{msg.dx, msg.dy, msg.dz};
    const Ray  ray{origin, dir};

    // 1. Closest wall along the shot.
    float worldT = HitscanRifle::kMaxRange;
    bool  worldHit = false;
    for (const auto& box : worldBoxes) {
        float t = 0.0f;
        Vec3 n{};
        if (intersectRayAabb(ray, box, t, n) && t >= 0.0f && t < worldT) {
            worldT = t;
            worldHit = true;
        }
    }

    // 2. Closest lag-compensated player.
    const auto playerHit = lagComp.hitscan(
        shooterPeerId, origin, dir, msg.viewTimeSec, playerHalfExtents, alivePeers);

    if (!playerHit) return out;
    if (worldHit && worldT < playerHit->distance) return out;
    if (playerHit->distance > HitscanRifle::kMaxRange) return out;

    DamageMsg dm{};
    dm.attackerPeerId = shooterPeerId;
    dm.victimPeerId   = playerHit->peerId;
    dm.damage         = static_cast<std::uint16_t>(HitscanRifle::kDamage);
    dm.victimHpAfter  = 0xFFFFu;
    out.damage = dm;
    return out;
}

}  // namespace iron::netshooter
