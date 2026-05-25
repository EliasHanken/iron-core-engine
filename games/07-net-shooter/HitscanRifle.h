#pragma once

#include "Messages.h"

#include "game/WeaponCooldown.h"
#include "math/Aabb.h"
#include "math/Vec.h"
#include "net/LagCompensator.h"

#include <cstdint>
#include <optional>
#include <span>

namespace iron::netshooter {

struct HitscanRifle {
    static constexpr float kCooldownSec = 0.5f;     // 2 shots/sec
    static constexpr int   kDamage      = 35;
    static constexpr float kMaxRange    = 100.0f;
    WeaponCooldown cooldown{kCooldownSec};
};

// CLIENT: gate by cooldown, emit a FireHitscanMsg if allowed.
std::optional<FireHitscanMsg> tryFireHitscanClient(
    HitscanRifle& w, double nowSec,
    const Vec3& muzzle, const Vec3& aimDir, double viewTimeSec);

struct HitscanResolution {
    std::optional<DamageMsg> damage;
};

// HOST: server-side rate-limit + raycast world AND lag-compensated
// players. Returns a DamageMsg only if a player was hit AND was closer
// than the nearest wall. The returned msg has victimHpAfter left at
// 0xFFFF — caller fills it via applyDamageHost.
HitscanResolution resolveHitscanHost(
    HitscanRifle& serverWeapon, double nowSec,
    std::uint32_t shooterPeerId, const FireHitscanMsg& msg,
    const LagCompensator& lagComp,
    std::span<const Aabb> worldBoxes,
    std::span<const std::uint32_t> alivePeers,
    const Vec3& playerHalfExtents);

}  // namespace iron::netshooter
