#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/CharacterAnimator.h"
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

// Clip that translates bone 0 from x=0 at t=0 to x=10 at t=1.
AnimationClip makeTranslateClip(const char* name) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs  = {0.0f, 1.0f};
    s.outputs = {0, 0, 0,  10, 0, 0};
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
    // Test 1: switching to a registered state plays its clip.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.switchTo("walk");
        a.update(0.5f);
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);
        CHECK(a.currentState() == "walk");
    }

    // Test 2: switchTo(currentState) is a no-op; clip time keeps advancing.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.switchTo("walk");
        a.update(0.5f);   // time = 0.5
        a.switchTo("walk");  // SAME state; time must NOT reset
        a.update(0.25f);  // time = 0.75
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 7.5f);
    }

    // Test 3: switchTo(differentState) resets clip time to 0.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");
        const AnimationClip run  = makeTranslateClip("run");

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.setClipForState("run",  &run);
        a.switchTo("walk");
        a.update(0.5f);
        a.switchTo("run");  // DIFFERENT state; time resets
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);
        CHECK(a.currentState() == "run");
    }

    // Test 4: null clip => bind-pose palette (no crash).
    {
        const Skeleton sk = makeTrivialSkeleton();

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("idle", nullptr);
        a.switchTo("idle");
        a.update(1.0f);
        std::array<Mat4, 1> out{};
        out[0] = Mat4::identity();
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);
    }

    // Test 5: unknown state name => bind-pose fallback (no crash; warns once).
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.switchTo("jump");  // unregistered
        a.update(0.5f);
        std::array<Mat4, 1> out{};
        out[0] = Mat4::identity();
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);
        CHECK(a.currentState() == "jump");
    }

    return iron_test_result();
}
