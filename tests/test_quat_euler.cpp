#include "test_framework.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cmath>

using namespace iron;

int main() {
    // eulerToQuat(0,0,0) is identity.
    {
        const Quat q = eulerToQuat(Vec3{0.0f, 0.0f, 0.0f});
        CHECK_NEAR(q.x, 0.0f);
        CHECK_NEAR(q.y, 0.0f);
        CHECK_NEAR(q.z, 0.0f);
        CHECK_NEAR(q.w, 1.0f);
    }

    // Round-trip: euler -> quat -> euler returns the same angles (degrees),
    // for angles safely away from gimbal-lock (|pitch| != 90).
    {
        const Vec3 e{30.0f, 45.0f, -60.0f};
        const Quat q = eulerToQuat(e);
        const Vec3 r = quatToEuler(q);
        CHECK_NEAR(r.x, e.x);
        CHECK_NEAR(r.y, e.y);
        CHECK_NEAR(r.z, e.z);
    }

    // A known single-axis rotation: 90 deg about Y == fromAxisAngle(Y, pi/2).
    {
        const Quat q = eulerToQuat(Vec3{0.0f, 90.0f, 0.0f});
        const Quat e = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5707963f);
        CHECK_NEAR(q.x, e.x);
        CHECK_NEAR(q.y, e.y);
        CHECK_NEAR(q.z, e.z);
        CHECK_NEAR(q.w, e.w);
    }

    // quatToEuler(identity) is zero.
    {
        const Vec3 r = quatToEuler(Quat::identity());
        CHECK_NEAR(r.x, 0.0f);
        CHECK_NEAR(r.y, 0.0f);
        CHECK_NEAR(r.z, 0.0f);
    }

    return iron_test_result();
}
