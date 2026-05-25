#include "RocketLauncher.h"

#include <algorithm>
#include <cmath>

namespace iron::netshooter {

static float distAabbToPoint(const Aabb& box, const Vec3& p) {
    const float cx = std::clamp(p.x, box.min.x, box.max.x);
    const float cy = std::clamp(p.y, box.min.y, box.max.y);
    const float cz = std::clamp(p.z, box.min.z, box.max.z);
    const float dx = p.x - cx;
    const float dy = p.y - cy;
    const float dz = p.z - cz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::optional<FireRocketMsg> tryFireRocketClient(
    RocketLauncher& w, double nowSec,
    const Vec3& muzzle, const Vec3& aimDir, double viewTimeSec) {
    if (!w.cooldown.tryFire(nowSec)) return std::nullopt;
    return FireRocketMsg{muzzle.x, muzzle.y, muzzle.z,
                         aimDir.x, aimDir.y, aimDir.z,
                         viewTimeSec};
}

std::optional<RocketSpawn> spawnRocketHost(
    RocketLauncher& serverWeapon, double nowSec,
    std::uint32_t shooterPeerId, const FireRocketMsg& msg,
    std::uint32_t projectileId,
    const Vec3& authoritativeShooterPos,
    float maxMuzzleDistance) {
    const float dx = msg.ox - authoritativeShooterPos.x;
    const float dy = msg.oy - authoritativeShooterPos.y;
    const float dz = msg.oz - authoritativeShooterPos.z;
    if (dx * dx + dy * dy + dz * dz > maxMuzzleDistance * maxMuzzleDistance) {
        return std::nullopt;
    }
    if (!serverWeapon.cooldown.tryFire(nowSec)) return std::nullopt;

    Projectile p;
    p.id = projectileId;
    p.ownerPeerId = shooterPeerId;
    p.position = Vec3{msg.ox, msg.oy, msg.oz};
    p.velocity = Vec3{msg.dx * RocketLauncher::kMuzzleSpeed,
                      msg.dy * RocketLauncher::kMuzzleSpeed,
                      msg.dz * RocketLauncher::kMuzzleSpeed};
    p.spawnTimeSec = nowSec;
    p.alive = true;

    RocketSpawn out;
    out.announce.projectileId = projectileId;
    out.announce.ownerPeerId  = shooterPeerId;
    out.announce.x = msg.ox; out.announce.y = msg.oy; out.announce.z = msg.oz;
    out.announce.vx = p.velocity.x; out.announce.vy = p.velocity.y; out.announce.vz = p.velocity.z;
    out.announce.spawnTimeSec = nowSec;
    out.live = p;
    return out;
}

RocketTickResult tickRocketHost(
    Projectile& live, double nowSec, float dt,
    const LagCompensator& lagComp,
    std::span<const Aabb> worldBoxes,
    std::span<const std::uint32_t> alivePeers,
    const Vec3& playerHalfExtents) {
    RocketTickResult out;
    if (!live.alive) return out;

    const float ageSec = static_cast<float>(nowSec - live.spawnTimeSec);
    const bool  lifetimeExpired = ageSec >= RocketLauncher::kMaxLifetimeSec;

    const auto hit = tickProjectile(live, dt, worldBoxes);

    if (!hit.has_value() && !lifetimeExpired) {
        return out;  // still flying
    }

    // Detonate at live.position (which tickProjectile already snapped
    // to the impact point on a world hit; on lifetime expiry it's the
    // last advanced position).
    out.expired = true;
    out.despawn.projectileId = live.id;
    out.despawn.x = live.position.x;
    out.despawn.y = live.position.y;
    out.despawn.z = live.position.z;
    live.alive = false;

    // Splash damage against lag-compensated AABBs at nowSec.
    for (const auto pid : alivePeers) {
        const auto aabb = lagComp.aabbAt(pid, nowSec, playerHalfExtents);
        if (!aabb) continue;
        if (!sphereOverlapAabb(live.position, RocketLauncher::kSplashRadius, *aabb)) continue;
        const float d = distAabbToPoint(*aabb, live.position);
        const float t = std::clamp(d / RocketLauncher::kSplashRadius, 0.0f, 1.0f);
        const int dmg = static_cast<int>(
            std::round(RocketLauncher::kCenterDamage * (1.0f - t)));
        if (dmg <= 0) continue;
        DamageMsg dm{};
        dm.attackerPeerId = live.ownerPeerId;
        dm.victimPeerId   = pid;
        dm.damage         = static_cast<std::uint16_t>(dmg);
        dm.victimHpAfter  = 0xFFFFu;  // caller fills via applyDamageHost
        out.splashHits.push_back(dm);
    }
    return out;
}

void tickRocketClient(Projectile& ghost, float dt,
                      std::span<const Aabb> worldBoxes) {
    if (!ghost.alive) return;
    // Same swept collision, but on hit we stop motion and leave alive=true.
    // The host's DespawnProjectileMsg will be authoritative for removal.
    const Vec3 startPos = ghost.position;
    const auto hit = tickProjectile(ghost, dt, worldBoxes);
    if (hit.has_value()) {
        // tickProjectile already set alive=false; flip back to true so
        // the visual sticks until host says despawn.
        ghost.alive = true;
        ghost.position = hit->point;
        ghost.velocity = Vec3{0.0f, 0.0f, 0.0f};
    }
    (void)startPos;
}

}  // namespace iron::netshooter
