#include "test_framework.h"
#include "RopeWalker.h"
#include "math/Aabb.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <cmath>
#include <vector>

using namespace iron;

int main() {
    // hasFooting: inside an island footprint is true; over the gap is false.
    {
        std::vector<Aabb> islands = {
            Aabb{Vec3{-10, -1, -10}, Vec3{10, 1, 10}},
        };
        CHECK(hasFooting(0.0f, 0.0f, islands));     // centre
        CHECK(hasFooting(9.0f, -9.0f, islands));    // inside a corner
        CHECK(!hasFooting(0.0f, -25.0f, islands));  // over the gap
        CHECK(!hasFooting(15.0f, 0.0f, islands));   // past the x edge
    }

    // leanDriftMagnitude grows with time on the rope, then caps.
    {
        CHECK(leanDriftMagnitude(1.0f) > leanDriftMagnitude(0.0f));
        // The cap holds: a very long time does not blow past the maximum.
        CHECK_NEAR(leanDriftMagnitude(1000.0f), leanDriftMagnitude(100.0f));
    }

    // applyLean: an off-centre lean with no input self-amplifies; a
    // counter-steer opposing the lean shrinks it.
    {
        const float grown = applyLean(0.5f, 0.0f, 0.0f, 0.1f);
        CHECK(grown > 0.5f);
        const float corrected = applyLean(0.5f, 0.0f, -1.0f, 0.1f);
        CHECK(corrected < 0.5f);
        // A pure nudge with no lean and no steer moves lean by the nudge.
        CHECK_NEAR(applyLean(0.0f, 0.05f, 0.0f, 0.1f), 0.05f);
    }

    // advanceParam: clamped to [0,1]; forward/back move t the expected way;
    // a non-positive rope length is a no-op.
    {
        CHECK_NEAR(advanceParam(0.0f, 0.0f, 3.0f, 10.0f, 0.1f), 0.0f);
        CHECK(advanceParam(0.5f, 1.0f, 3.0f, 10.0f, 0.1f) > 0.5f);
        CHECK(advanceParam(0.5f, -1.0f, 3.0f, 10.0f, 0.1f) < 0.5f);
        CHECK_NEAR(advanceParam(1.0f, 1.0f, 3.0f, 10.0f, 1.0f), 1.0f);  // clamp hi
        CHECK_NEAR(advanceParam(0.0f, -1.0f, 3.0f, 10.0f, 1.0f), 0.0f); // clamp lo
        CHECK_NEAR(advanceParam(0.5f, 1.0f, 3.0f, 0.0f, 0.1f), 0.5f);   // no-op
    }

    // findMountRope: a player near a rope's first point gets that rope index
    // with outAtStart true; far from any rope gives -1.
    {
        std::vector<Rope> ropes;
        ropes.push_back(Rope(Vec3{0, 0, 0}, Vec3{10, 0, 0}, 8, 12.0f));
        bool atStart = false;
        const int near = findMountRope(Vec3{0.3f, 0.0f, 0.2f}, ropes, 1.5f,
                                       atStart);
        CHECK(near == 0);
        CHECK(atStart);
        const int nearEnd = findMountRope(Vec3{10.2f, 0.0f, 0.0f}, ropes, 1.5f,
                                          atStart);
        CHECK(nearEnd == 0);
        CHECK(!atStart);
        CHECK(findMountRope(Vec3{50, 0, 50}, ropes, 1.5f, atStart) == -1);
        // Distance is XZ-only: a Y offset does not prevent mounting.
        const int yOffset =
            findMountRope(Vec3{0.3f, 5.0f, 0.2f}, ropes, 1.5f, atStart);
        CHECK(yOffset == 0);
    }

    return iron_test_result();
}
