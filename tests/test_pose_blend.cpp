#include "asset/Pose.h"
#include "asset/PoseBlend.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

namespace {
Pose makePose(Vec3 t, Quat r, Vec3 s) {
    Pose p;
    p.bones.push_back(BoneLocal{t, r, s});
    return p;
}
}  // namespace

int main() {
    const Pose a = makePose(Vec3{0, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
    const Pose b = makePose(Vec3{10, 0, 0},
                            Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5707963f),
                            Vec3{2, 2, 2});

    // t=0 -> a
    {
        Pose out;
        blendPose(a, b, 0.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 0.0f);
        CHECK_NEAR(out.bones[0].scale.x, 1.0f);
    }
    // t=1 -> b
    {
        Pose out;
        blendPose(a, b, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
        CHECK_NEAR(out.bones[0].scale.x, 2.0f);
    }
    // t=0.5 -> midpoint T/S, slerp R (45 deg about +Y)
    {
        Pose out;
        blendPose(a, b, 0.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
        CHECK_NEAR(out.bones[0].scale.x, 1.5f);
        // 45 deg about +Y rotates +X toward -Z by 45 deg: x=cos45, z=-sin45.
        const Vec3 v = out.bones[0].rotation.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.70710678f);
        CHECK_NEAR(v.z, -0.70710678f);
    }
    // t clamps below 0 and above 1.
    {
        Pose lo, hi;
        blendPose(a, b, -1.0f, lo);
        blendPose(a, b, 2.0f, hi);
        CHECK_NEAR(lo.bones[0].translation.x, 0.0f);
        CHECK_NEAR(hi.bones[0].translation.x, 10.0f);
    }

    return iron_test_result();
}
