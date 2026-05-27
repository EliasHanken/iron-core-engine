// Compiles the game-side RocketLauncher.cpp directly (mirrors test_rope_walker).
//
// M20: host-side rocket logic moved from RocketLauncher into main.cpp
// (Jolt-driven). RocketLauncher.cpp now only exposes the client-side
// surface: `tryFireRocketClient` (cooldown gate) and `tickRocketClient`
// (linear extrapolation of ghost rockets).

#include "test_framework.h"
#include "RocketLauncher.h"

#include <span>

using namespace iron;
using namespace iron::netshooter;

int main() {
    // tryFireRocketClient: cooldown gates rapid fire.
    {
        RocketLauncher w;
        const Vec3 muzzle{0, 1, 0};
        const Vec3 dir{1, 0, 0};
        const auto first = tryFireRocketClient(w, 0.0, muzzle, dir, 0.0);
        CHECK(first.has_value());
        const auto blocked = tryFireRocketClient(w, 0.5, muzzle, dir, 0.0);
        CHECK(!blocked.has_value());
        // After cooldown, fires again.
        const auto third = tryFireRocketClient(
            w, RocketLauncher::kCooldownSec + 0.01, muzzle, dir, 0.0);
        CHECK(third.has_value());
        CHECK_NEAR(third->ox, muzzle.x);
        CHECK_NEAR(third->dx, dir.x);
    }

    // tickRocketClient: linear extrapolation, ignores world boxes.
    {
        Projectile ghost;
        ghost.position = Vec3{0.0f, 1.0f, 0.0f};
        ghost.velocity = Vec3{10.0f, 0.0f, 0.0f};
        ghost.alive = true;
        tickRocketClient(ghost, /*dt=*/0.1f, std::span<const Aabb>{});
        CHECK_NEAR(ghost.position.x, 1.0f);
        CHECK_NEAR(ghost.position.y, 1.0f);
        CHECK(ghost.alive);
    }

    // tickRocketClient: dead ghosts don't move.
    {
        Projectile ghost;
        ghost.position = Vec3{0.0f, 1.0f, 0.0f};
        ghost.velocity = Vec3{10.0f, 0.0f, 0.0f};
        ghost.alive = false;
        tickRocketClient(ghost, 0.1f, std::span<const Aabb>{});
        CHECK_NEAR(ghost.position.x, 0.0f);
    }

    return iron_test_result();
}
