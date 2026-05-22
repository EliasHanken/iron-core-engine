#include "test_framework.h"
#include "RopeWalker.h"
#include "math/Aabb.h"
#include "math/Mat4.h"
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

    // A straight rope on the x axis, 8 segments. The far end (10,0,0) sits on
    // a "far island" AABB; the near end (0,0,0) does not.
    auto makeRope = []() {
        return Rope(Vec3{0, 0, 0}, Vec3{10, 0, 0}, 8, 10.5f);
    };
    const std::vector<Aabb> farIsland = {
        Aabb{Vec3{8, -1, -3}, Vec3{13, 1, 3}},
    };
    const float dt = 1.0f / 60.0f;

    // Walking the rope fully forward, with steady counter-steering, reaches
    // the far island and wins.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            // Counter-steer toward zero each step to hold balance.
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Won);
    }

    // No counter-steering and a steady perturbation: the player falls.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            result = walker.step(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Fell);
    }

    // Retreating off the start end dismounts without a win.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        // Step forward a little so t is clearly above 0.
        for (int i = 0; i < 20; ++i) {
            walker.step(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, dt, rope, farIsland);
        }
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(-1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Dismounted);
    }

    // Crossing fully when the far end is NOT on the win island dismounts,
    // does not win.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        const std::vector<Aabb> noIsland;  // empty: nothing counts as a win
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  noIsland);
        }
        CHECK(result == RopeWalker::Result::Dismounted);
    }

    // With no movement input, the walker stays on the rope (it does not
    // immediately dismount at t=0).
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        const RopeWalker::Result result =
            walker.step(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, dt, rope, farIsland);
        CHECK(result == RopeWalker::Result::Traversing);
    }

    // Reverse mount: begun at the rope's far point (atStart=false). Walking
    // forward then runs t toward the rope's FIRST point (0,0,0), which is not
    // on farIsland — so a full crossing dismounts rather than wins. This
    // exercises the atStart_=false branches of sampleRope/mountEndPoint/
    // farEndPoint.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, false, 0.0f, 0.0f);
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Dismounted);
    }

    return iron_test_result();
}
