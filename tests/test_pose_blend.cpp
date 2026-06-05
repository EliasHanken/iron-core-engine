#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/PoseBlend.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
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
Skeleton oneBoneSkeleton() {
    Skeleton sk;
    Bone b;
    b.parentIndex = -1;
    b.localBindTransform = Mat4::identity();
    b.inverseBindMatrix = Mat4::identity();
    sk.bones.push_back(b);
    return sk;
}
AnimationClip slideClip(float endX) {
    AnimationClip c;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs = {0.0f, 1.0f};
    s.outputs = {0, 0, 0, endX, 0, 0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch;
    ch.targetBone = 0;
    ch.path = AnimationPath::Translation;
    ch.samplerIndex = 0;
    c.channels.push_back(ch);
    return c;
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

    {
        const Skeleton sk = oneBoneSkeleton();
        const AnimationClip walk = slideClip(0.0f);   // at param 0, bone x stays 0
        const AnimationClip run  = slideClip(10.0f);  // at param 1, bone x -> 10 at t=1

        BlendSpace1D space;
        space.add(1.0f, &run);   // add out of order on purpose
        space.add(0.0f, &walk);
        CHECK(space.samples.size() == 2);
        CHECK_NEAR(space.samples[0].first, 0.0f);  // sorted ascending

        Pose out;
        // midpoint param at t=1: blend of x=0 and x=10 -> x=5
        sampleBlendSpace(sk, space, 0.5f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
        // param below range clamps to walk (x=0)
        sampleBlendSpace(sk, space, -3.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 0.0f);
        // param above range clamps to run (x=10)
        sampleBlendSpace(sk, space, 5.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
        // exact hit on run param
        sampleBlendSpace(sk, space, 1.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
    }

    return iron_test_result();
}
