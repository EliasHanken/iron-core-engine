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
AnimationClip slideClipDur(float endX, float dur) {
    AnimationClip c;
    c.duration = dur;
    AnimationSampler s;
    s.inputs = {0.0f, dur};
    s.outputs = {0, 0, 0, endX, 0, 0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch;
    ch.targetBone = 0; ch.path = AnimationPath::Translation; ch.samplerIndex = 0;
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
        const AnimationClip walk = slideClip(0.0f);   // dur 1.0, x stays 0
        const AnimationClip run  = slideClip(10.0f);  // dur 1.0, x:0->10
        BlendSpace1D space;
        space.add(1.0f, &run);   // out of order on purpose
        space.add(0.0f, &walk);
        CHECK(space.samples.size() == 2);
        CHECK_NEAR(space.samples[0].first, 0.0f);  // sorted ascending

        Pose out;
        // midpoint param, phase 0.5 (time 0.5, dur 1.0): run x=5, walk x=0 -> 2.5
        sampleBlendSpace(sk, space, 0.5f, 0.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 2.5f);
        // param below range clamps to walk (x stays 0)
        sampleBlendSpace(sk, space, -3.0f, 0.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 0.0f);
        // param above range clamps to run; phase 0.5 -> x=5
        sampleBlendSpace(sk, space, 5.0f, 0.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
        // FREEZE REGRESSION: time (2.5) far beyond clip duration (1.0) must LOOP,
        // not clamp to the last frame. phase = fmod(2.5,1.0)/1.0 = 0.5 -> x=5.
        sampleBlendSpace(sk, space, 5.0f, 2.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
    }
    // Phase-sync across clips of DIFFERENT durations: both sampled at the same
    // normalized phase, not the same absolute time.
    {
        const Skeleton sk = oneBoneSkeleton();
        // Non-proportional slopes so phase-sync differs from absolute-time
        // sampling (shortC slope 10/s, longC slope 20/s).
        const AnimationClip shortC = slideClipDur(10.0f, 1.0f);  // x:0->10 over 1s
        const AnimationClip longC  = slideClipDur(40.0f, 2.0f);  // x:0->40 over 2s
        BlendSpace1D space;
        space.add(0.0f, &shortC);
        space.add(1.0f, &longC);
        Pose out;
        // param 0.5 -> w=0.5, blendedDur = 1*0.5 + 2*0.5 = 1.5.
        // phase = 1.0/1.5 = 2/3. shortC@(2/3*1=0.667)->x=6.667;
        // longC@(2/3*2=1.333)->(1.333/2)*40=26.667. blend w=0.5 -> 16.667.
        // (Absolute-time sampling would give 15: shortC clamps to x=10 at end.)
        sampleBlendSpace(sk, space, 0.5f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 16.6666667f);
    }

    return iron_test_result();
}
