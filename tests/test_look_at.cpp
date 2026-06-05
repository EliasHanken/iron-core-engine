#include "asset/Ik.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace iron;

int main() {
    // Unclamped: forward +X turned toward target at -Z (90 deg) lands on -Z.
    {
        const Quat q = solveLookAt(Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                   Vec3{0, 0, -1}, 3.14159f);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.0f);
        CHECK_NEAR(v.z, -1.0f);
    }
    // Clamped: same geometry but limited to 0.2 rad -> result turns exactly
    // 0.2 rad (angle between forward and rotated-forward == 0.2).
    {
        const float maxA = 0.2f;
        const Quat q = solveLookAt(Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                   Vec3{0, 0, -1}, maxA);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        const float ang = std::acos(std::clamp(dot(normalize(v), Vec3{1, 0, 0}),
                                               -1.0f, 1.0f));
        CHECK_NEAR(ang, maxA);
    }
    // Coincident target -> identity (forward unchanged).
    {
        const Quat q = solveLookAt(Vec3{2, 2, 2}, Vec3{1, 0, 0},
                                   Vec3{2, 2, 2}, 1.0f);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 1.0f);
        CHECK_NEAR(v.y, 0.0f);
        CHECK_NEAR(v.z, 0.0f);
    }

    return iron_test_result();
}
