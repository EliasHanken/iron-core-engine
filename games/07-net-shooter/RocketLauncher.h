#pragma once

#include "Messages.h"

#include "game/Collision.h"
#include "game/ProjectileSim.h"
#include "game/WeaponCooldown.h"
#include "math/Aabb.h"
#include "math/Vec.h"
#include "net/LagCompensator.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

struct RocketSpawn {
    SpawnProjectileMsg announce;
    Projectile         live;
};

// HOST: validate the spoof-resistance check (muzzle within
// `maxMuzzleDistance` of the shooter's authoritative position), gate by
// host cooldown, return the SpawnProjectileMsg + the seeded Projectile
// to add to host state.
std::optional<RocketSpawn> spawnRocketHost(
    RocketLauncher& serverWeapon, double nowSec,
    std::uint32_t shooterPeerId, const FireRocketMsg& msg,
    std::uint32_t projectileId,
    const Vec3& authoritativeShooterPos,
    float maxMuzzleDistance = 2.0f);

struct RocketTickResult {
    bool                   expired = false;
    DespawnProjectileMsg   despawn{};
    std::vector<DamageMsg> splashHits;
};

// HOST: advance `live` by dt. World hit OR lifetime expiry sets
// expired=true and fills `despawn` (the broadcast message) and
// `splashHits` (one DamageMsg per alive peer in splashRadius). Splash
// uses linear falloff and lag-compensated AABBs at the detonation tick.
RocketTickResult tickRocketHost(
    Projectile& live, double nowSec, float dt,
    const LagCompensator& lagComp,
    std::span<const Aabb> worldBoxes,
    std::span<const std::uint32_t> alivePeers,
    const Vec3& playerHalfExtents);

// CLIENT: visual-only ghost tick — moves forward, stops on world hit
// but does NOT despawn (waits for DespawnProjectileMsg).
void tickRocketClient(Projectile& ghost, float dt,
                      std::span<const Aabb> worldBoxes);

}  // namespace iron::netshooter
