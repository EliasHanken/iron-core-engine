#include "test_framework.h"
#include "common/WeaponCooldown.h"

using namespace iron;

int main() {
    // Fresh cooldown: first fire at t=0 succeeds, immediate second fails.
    {
        WeaponCooldown c{0.5f};
        CHECK(c.tryFire(0.0));
        CHECK(!c.tryFire(0.0));
        CHECK(!c.tryFire(0.49));
        CHECK(c.tryFire(0.5));   // exactly at the boundary: success
    }

    // timeUntilReady reports correctly mid-cooldown and clamps to 0.
    {
        WeaponCooldown c{1.0f};
        CHECK(c.tryFire(10.0));               // nextFireAt = 11.0
        CHECK_NEAR(c.timeUntilReady(10.25), 0.75f);
        CHECK_NEAR(c.timeUntilReady(11.0), 0.0f);
        CHECK_NEAR(c.timeUntilReady(99.0), 0.0f);  // clamped
    }

    // reset() makes the cooldown immediately ready again.
    {
        WeaponCooldown c{1.0f};
        CHECK(c.tryFire(5.0));
        CHECK(!c.tryFire(5.5));
        c.reset();
        CHECK(c.tryFire(5.5));
    }

    return iron_test_result();
}
