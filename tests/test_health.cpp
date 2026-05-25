#include "test_framework.h"
#include "game/Health.h"

using namespace iron;

int main() {
    // applyDamage clamps at 0; isAlive flips at 0; resetHealth restores.
    {
        Health h{100, 100};
        CHECK(isAlive(h));
        applyDamage(h, 30);
        CHECK(h.current == 70);
        CHECK(isAlive(h));
        applyDamage(h, 1000);
        CHECK(h.current == 0);
        CHECK(!isAlive(h));
        resetHealth(h);
        CHECK(h.current == 100);
        CHECK(isAlive(h));
    }

    // Negative damage is ignored (no healing).
    {
        Health h{100, 50};
        applyDamage(h, -25);
        CHECK(h.current == 50);
    }

    // Zero-max edge case is well-defined (already dead).
    {
        Health h{0, 0};
        CHECK(!isAlive(h));
        applyDamage(h, 10);
        CHECK(h.current == 0);
    }

    return iron_test_result();
}
