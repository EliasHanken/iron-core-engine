// Compiles the game-side RocketLauncher.cpp directly (mirrors test_rope_walker).

#include "test_framework.h"
#include "RocketLauncher.h"

#include <array>

using namespace iron;
using namespace iron::netshooter;

int main() {
    const Vec3 half = kPlayerHalfExtents;

    // spawnRocketHost: spawn time + velocity sanity.
    {
        RocketLauncher w;
        const FireRocketMsg msg{0, 0, 0,  1, 0, 0,  0.0};
        const auto spawn = spawnRocketHost(
            w, /*nowSec=*/100.0, /*shooter=*/1, msg,
            /*projectileId=*/42, /*authShooter=*/Vec3{0, 0, 0});
        CHECK(spawn.has_value());
        CHECK_NEAR(static_cast<float>(spawn->announce.spawnTimeSec), 100.0f);
        CHECK_NEAR(spawn->announce.vx, RocketLauncher::kMuzzleSpeed);
        CHECK_NEAR(spawn->announce.vy, 0.0f);
        CHECK(spawn->announce.projectileId == 42);
    }

    // Spoof: muzzle 5 m from authoritative shooter pos → rejected.
    {
        RocketLauncher w;
        const FireRocketMsg msg{5.0f, 0, 0,  1, 0, 0,  0.0};
        const auto spawn = spawnRocketHost(
            w, 0.0, 1, msg, 1, Vec3{0, 0, 0});
        CHECK(!spawn.has_value());
    }

    // Splash falloff at radius/2 ≈ half damage.
    {
        Projectile p;
        p.id = 1;
        p.ownerPeerId = 1;
        p.position = Vec3{0.0f, 1.0f, 0.0f};
        p.velocity = Vec3{0.1f, 0.0f, 0.0f};
        p.spawnTimeSec = 0.0;
        p.alive = true;

        // Force the rocket to expire this tick: nowSec >> spawnTimeSec
        // + lifetime. Place a target player 2 m away from detonation.
        LagCompensator lc;
        lc.recordPosition(2, 999.0, Vec3{2.0f, 1.0f, 0.0f});
        const std::array<std::uint32_t, 1> alive{2};

        const auto res = tickRocketHost(
            p, /*nowSec=*/999.0, /*dt=*/0.001f,
            lc,
            std::span<const Aabb>{},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(res.expired);
        CHECK(res.splashHits.size() == 1);
        // Player AABB closest point to detonation: x-half=1.6, y is in
        // range, z is in range → distance to surface ≈ 1.6.
        // Falloff t = 1.6 / 4.0 = 0.4, damage = round(80 * 0.6) = 48.
        CHECK(res.splashHits[0].damage == 48);
        CHECK(res.splashHits[0].attackerPeerId == 1);
        CHECK(res.splashHits[0].victimPeerId == 2);
    }

    // Self-damage: shooter standing on detonation point appears in
    // splashHits at center damage.
    {
        Projectile p;
        p.id = 1;
        p.ownerPeerId = 1;
        p.position = Vec3{0.0f, 1.0f, 0.0f};
        p.velocity = Vec3{0.1f, 0.0f, 0.0f};
        p.spawnTimeSec = 0.0;
        p.alive = true;
        LagCompensator lc;
        lc.recordPosition(1, 999.0, Vec3{0.0f, 1.0f, 0.0f});
        const std::array<std::uint32_t, 1> alive{1};

        const auto res = tickRocketHost(
            p, 999.0, 0.001f, lc,
            std::span<const Aabb>{},
            std::span<const std::uint32_t>{alive},
            half);
        CHECK(res.expired);
        CHECK(res.splashHits.size() == 1);
        CHECK(res.splashHits[0].victimPeerId == 1);
        CHECK(res.splashHits[0].damage == 80);
    }

    return iron_test_result();
}
