#include "asset/Ik.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

int main() {
    // Reachable target: end reaches target; segment lengths preserved; mid
    // bends toward +Y (pole). Chain root(0,0,0)-mid(1,0,0)-end(2,0,0),
    // lengths 1 and 1. Target (1,1,0) is reachable (max reach ~2).
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const Vec3 target{1, 1, 0}, pole{0, 1, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, target, pole);
        CHECK_NEAR(r.endPos.x, 1.0f);
        CHECK_NEAR(r.endPos.y, 1.0f);
        CHECK_NEAR(r.endPos.z, 0.0f);
        CHECK_NEAR(length(r.midPos - root), 1.0f);          // lab preserved
        CHECK_NEAR(length(r.endPos - r.midPos), 1.0f);       // lcb preserved
        CHECK(r.midPos.y > 0.0f);                            // bent toward pole
    }

    // Unreachable target: chain fully extends toward target, no NaN.
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const Vec3 target{5, 0, 0}, pole{0, 1, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, target, pole);
        CHECK_NEAR(r.endPos.x, 2.0f);                        // ~ lab+lcb
        CHECK(std::isfinite(r.endPos.x));
        CHECK(std::isfinite(r.midPos.y));
        CHECK_NEAR(length(r.midPos - root), 1.0f);
    }

    // Degenerate: target at the root. Must not NaN.
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, root, Vec3{0, 1, 0});
        CHECK(std::isfinite(r.midPos.x));
        CHECK(std::isfinite(r.endPos.x));
    }

    // rotationFromTo basic: +X onto +Y.
    {
        const Quat q = rotationFromTo(Vec3{1, 0, 0}, Vec3{0, 1, 0});
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.0f);
        CHECK_NEAR(v.y, 1.0f);
    }

    // rotationFromTo parallel: same direction -> identity.
    {
        const Quat q = rotationFromTo(Vec3{1, 0, 0}, Vec3{2, 0, 0});
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 1.0f);
        CHECK_NEAR(v.y, 0.0f);
        CHECK_NEAR(v.z, 0.0f);
    }
    // rotationFromTo anti-parallel: opposite direction -> 180 deg (x flips).
    {
        const Quat q = rotationFromTo(Vec3{1, 0, 0}, Vec3{-1, 0, 0});
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, -1.0f);
    }

    return iron_test_result();
}
