#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <array>
#include <cmath>

using namespace iron;

namespace {

Skeleton makeTrivialSkeleton() {
    Skeleton s;
    Bone b;
    b.parentIndex = -1;
    b.inverseBindMatrix = Mat4::identity();
    b.localBindTransform = Mat4::identity();
    b.name = "root";
    s.bones.push_back(b);
    return s;
}

AnimationClip makeRotateClip() {
    AnimationClip clip;
    clip.name = "spin";
    clip.duration = 1.0f;

    AnimationSampler samp;
    samp.inputs  = {0.0f, 1.0f};
    const Quat q0 = Quat::identity();
    const Quat q1 = Quat::fromAxisAngle(Vec3{0, 1, 0}, 3.14159265f * 0.5f);
    samp.outputs = {q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w};
    samp.interpolation = AnimationInterpolation::Linear;
    clip.samplers.push_back(samp);

    AnimationChannel ch;
    ch.targetBone   = 0;
    ch.path         = AnimationPath::Rotation;
    ch.samplerIndex = 0;
    clip.channels.push_back(ch);

    return clip;
}

}  // namespace

int main() {
    // No skeleton => evaluate is a no-op; caller-initialized matrices remain.
    {
        AnimationPlayer p;
        std::array<Mat4, 4> out{};
        for (auto& m : out) m = Mat4::identity();
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
    }

    // Skeleton set but no clip => write bind-pose palette (identity here).
    {
        const Skeleton sk = makeTrivialSkeleton();
        AnimationPlayer p;
        p.setSkeleton(&sk);
        std::array<Mat4, 1> out{};
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
    }

    // Clip at t=0 => bind pose (identity rotation).
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip clip = makeRotateClip();
        AnimationPlayer p;
        p.setSkeleton(&sk);
        p.setClip(&clip);
        p.setTime(0.0f);
        std::array<Mat4, 1> out{};
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
    }

    // Clip at t=duration => 90deg Y rotation.
    // Column-major: rotating +X (1,0,0) by 90deg about +Y yields -Z (0,0,-1).
    // Matrix entries: at(0,0) = cos(theta) = 0; at(2,0) = -sin(theta) = -1.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip clip = makeRotateClip();
        AnimationPlayer p;
        p.setSkeleton(&sk);
        p.setClip(&clip);
        p.setTime(1.0f);
        std::array<Mat4, 1> out{};
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 0.0f);
        CHECK_NEAR(out[0].at(0, 2), 1.0f);
    }

    // update() wraps time at clip duration.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip clip = makeRotateClip();
        AnimationPlayer p;
        p.setSkeleton(&sk);
        p.setClip(&clip);
        p.setTime(0.0f);
        p.update(2.5f);
        CHECK_NEAR(p.time(), 0.5f);
    }

    // Step interpolation holds previous keyframe value until next key.
    {
        const Skeleton sk = makeTrivialSkeleton();
        AnimationClip clip;
        clip.name = "step";
        clip.duration = 2.0f;
        AnimationSampler samp;
        samp.inputs = {0.0f, 1.0f, 2.0f};
        samp.outputs = {0,0,0,  5,0,0,  10,0,0};
        samp.interpolation = AnimationInterpolation::Step;
        clip.samplers.push_back(samp);
        AnimationChannel ch;
        ch.targetBone   = 0;
        ch.path         = AnimationPath::Translation;
        ch.samplerIndex = 0;
        clip.channels.push_back(ch);

        AnimationPlayer p;
        p.setSkeleton(&sk);
        p.setClip(&clip);
        p.setTime(0.5f);
        std::array<Mat4, 1> out{};
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);

        p.setTime(1.99f);
        p.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);
    }

    // Empty sampler (loader pushes one for failed reads to preserve indices):
    // a channel pointing at an empty sampler must early-return identity/zero,
    // leaving the bone at its bind-pose value. This mirrors the loader
    // invariant on AnimationClip::samplers.
    {
        const Skeleton sk = makeTrivialSkeleton();
        AnimationClip clip;
        clip.name = "with-empty";
        clip.duration = 1.0f;

        // sampler 0: empty (simulates a failed-read sampler placeholder).
        clip.samplers.push_back(AnimationSampler{});

        // sampler 1: real translation track.
        AnimationSampler samp;
        samp.inputs = {0.0f, 1.0f};
        samp.outputs = {0,0,0,  7,0,0};
        samp.interpolation = AnimationInterpolation::Linear;
        clip.samplers.push_back(samp);

        // Channel referencing the empty sampler must be a no-op (identity).
        AnimationChannel chEmpty;
        chEmpty.targetBone = 0;
        chEmpty.path = AnimationPath::Rotation;
        chEmpty.samplerIndex = 0;
        clip.channels.push_back(chEmpty);

        // Channel referencing the real sampler must still drive its bone:
        // proves channel.samplerIndex stays aligned even with the empty
        // sampler in front of it.
        AnimationChannel chReal;
        chReal.targetBone = 0;
        chReal.path = AnimationPath::Translation;
        chReal.samplerIndex = 1;
        clip.channels.push_back(chReal);

        AnimationPlayer p;
        p.setSkeleton(&sk);
        p.setClip(&clip);
        p.setTime(1.0f);
        std::array<Mat4, 1> out{};
        p.evaluate(out);
        // Translation x should be 7 (from sampler 1, end keyframe).
        CHECK_NEAR(out[0].at(0, 3), 7.0f);
        // Rotation untouched by the empty sampler => identity diagonal.
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
    }

    return iron_test_result();
}
