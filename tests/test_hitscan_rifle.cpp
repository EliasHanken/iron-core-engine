// This test compiles the game-side HitscanRifle.cpp directly (the game
// is not part of ironcore). See test_rope_walker for the same pattern.

#include "test_framework.h"
#include "HitscanRifle.h"

#include <array>

using namespace iron;
using namespace iron::netshooter;

int main() {
    const Vec3 half = kPlayerHalfExtents;

    // Direct hit at empty world: returns a DamageMsg with kDamage.
    {
        HitscanRifle w;
        LagCompensator lc;
        lc.recordPosition(2, 0.0, Vec3{5.0f, 0.0f, 0.0f});  // victim
        const FireHitscanMsg msg{0, 0, 0,  1, 0, 0,  0.0};
        const std::array<std::uint32_t, 1> alive{2};
        const auto res = resolveHitscanHost(
            w, /*nowSec=*/100.0,
            /*shooter=*/1, msg, lc,
            std::span<const Aabb>{},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(res.damage.has_value());
        CHECK(res.damage->attackerPeerId == 1);
        CHECK(res.damage->victimPeerId == 2);
        CHECK(res.damage->damage == 35);
    }

    // Wall closer than the victim: no damage.
    {
        HitscanRifle w;
        LagCompensator lc;
        lc.recordPosition(2, 0.0, Vec3{10.0f, 0.0f, 0.0f});
        const std::array<Aabb, 1> walls{
            Aabb{Vec3{3.0f, -1.0f, -1.0f}, Vec3{4.0f, 2.0f, 1.0f}}};
        const FireHitscanMsg msg{0, 0, 0,  1, 0, 0,  0.0};
        const std::array<std::uint32_t, 1> alive{2};
        const auto res = resolveHitscanHost(
            w, 100.0, 1, msg, lc,
            std::span<const Aabb>{walls},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(!res.damage.has_value());
    }

    // Server-side cooldown: a second call inside kCooldownSec returns
    // no damage even on a perfect-hit shot.
    {
        HitscanRifle w;
        LagCompensator lc;
        lc.recordPosition(2, 0.0, Vec3{5.0f, 0.0f, 0.0f});
        const FireHitscanMsg msg{0, 0, 0,  1, 0, 0,  0.0};
        const std::array<std::uint32_t, 1> alive{2};
        const auto first = resolveHitscanHost(
            w, 0.0, 1, msg, lc,
            std::span<const Aabb>{},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(first.damage.has_value());
        const auto second = resolveHitscanHost(
            w, 0.1, 1, msg, lc,
            std::span<const Aabb>{},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(!second.damage.has_value());
    }

    return iron_test_result();
}
