#include "asset/Animation.h"
#include "asset/CharacterAnimator.h"
#include "asset/Ik.h"
#include "asset/Pose.h"
#include "asset/PoseBlend.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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

    // Cross-fade with fadeTime 0 behaves as a hard cut (matches Test 3).
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
        a.switchTo("run", 0.0f);  // explicit hard cut
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);
    }

    // Mid-fade blends between previous and new pose. walk holds x=0 at t=0;
    // run drives x: blend at fade weight 0.5 with run at its own time.
    {
        const Skeleton sk = makeTrivialSkeleton();
        // "stand": bone stays at x=0 (no channel drift at t=0).
        AnimationClip stand;
        stand.name = "stand";
        stand.duration = 1.0f;
        AnimationSampler ss;
        ss.inputs = {0.0f, 1.0f};
        ss.outputs = {0, 0, 0,  0, 0, 0};  // x stays 0
        ss.interpolation = AnimationInterpolation::Linear;
        stand.samplers.push_back(ss);
        AnimationChannel sc;
        sc.targetBone = 0; sc.path = AnimationPath::Translation; sc.samplerIndex = 0;
        stand.channels.push_back(sc);

        const AnimationClip run = makeTranslateClip("run");  // x: 0 -> 10 over 1s

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("stand", &stand);
        a.setClipForState("run", &run);
        a.switchTo("stand");
        a.update(0.1f);             // settle in stand
        a.switchTo("run", 1.0f);    // begin a 1s fade; run time starts at 0
        a.update(0.5f);             // fade weight 0.5; run time 0.5 -> x=5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        // blend(prev stand x=0, cur run x=5, w=0.5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }

    // After the fade completes, only the new state drives the pose.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip stand = makeTranslateClip("a");  // x:0->10 (named "a")
        const AnimationClip run   = makeTranslateClip("b");
        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("stand", &stand);
        a.setClipForState("run", &run);
        a.switchTo("stand");
        a.update(0.2f);
        a.switchTo("run", 0.5f);
        a.update(0.5f);   // fade fully elapses; run time 0.5 -> x=5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);  // pure run, no blend
    }

    // Regression: two faded switchTo calls with NO evaluate between them must
    // not leave the palette uninitialized. Before the fix, the second switchTo
    // froze a default-constructed (0-bone) lastPose_, so blendPose produced a
    // 0-bone pose and posePalette wrote nothing -> caller's palette untouched.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");
        const AnimationClip run  = makeTranslateClip("run");
        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.setClipForState("run",  &run);
        a.switchTo("walk");
        a.update(0.1f);
        a.switchTo("run", 1.0f);   // start a fade; no evaluate yet
        a.switchTo("walk", 1.0f);  // retarget mid-fade BEFORE any evaluate
        a.update(0.5f);
        // Sentinel: if evaluate writes nothing, this identity stays untouched.
        std::array<Mat4, 1> out{};
        out[0].at(0, 3) = -999.0f;
        a.evaluate(out);
        // The palette must have been written (real bone count): the sentinel
        // is gone and the value is finite, not the all-zero / untouched garbage.
        CHECK(out[0].at(0, 3) != -999.0f);
        CHECK(std::isfinite(out[0].at(0, 3)));
    }

    // Blend-space state: param drives the blended output.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");  // x:0->10
        AnimationClip still;
        still.name = "still";
        still.duration = 1.0f;
        AnimationSampler ss;
        ss.inputs = {0.0f, 1.0f};
        ss.outputs = {0, 0, 0, 0, 0, 0};
        ss.interpolation = AnimationInterpolation::Linear;
        still.samplers.push_back(ss);
        AnimationChannel sc;
        sc.targetBone = 0; sc.path = AnimationPath::Translation; sc.samplerIndex = 0;
        still.channels.push_back(sc);

        BlendSpace1D space;
        space.add(0.0f, &still);   // param 0 -> x stays 0
        space.add(1.0f, &walk);    // param 1 -> x up to 10

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setBlendSpaceForState("locomotion", std::move(space));
        a.switchTo("locomotion");
        a.setBlendParam(0.5f);
        a.update(1.0f);            // time wraps to 0 at duration 1
        a.setBlendParam(0.5f);
        a.update(0.5f);           // time 0.5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        // blendSpace(param 0.5): blend(still x=0, walk x=5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }

    // Look-at rotates the configured bone toward the target.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        Bone head;
        head.parentIndex = 0;
        head.localBindTransform = composeTRS(Vec3{0, 1, 0}, Quat::identity(), Vec3{1, 1, 1});
        head.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);
        sk.bones.push_back(head);

        AnimationClip idle;  // no channels -> bind pose
        idle.name = "idle";
        idle.duration = 1.0f;

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("idle", &idle);
        a.switchTo("idle");

        const int h = a.addLookAt(1, Vec3{1, 0, 0}, 3.14159f, 1.0f);
        a.setLookAtTarget(h, Vec3{2, 0, 5});  // off to +Z from the head
        a.update(0.016f);

        std::array<Mat4, 2> palette{};
        std::array<Mat4, 2> noik{};
        a.setLookAtWeight(h, 0.0f);
        a.evaluate(noik);
        a.setLookAtWeight(h, 1.0f);
        a.evaluate(palette);
        // With look-at active, the head palette differs from the no-IK palette.
        bool differs = false;
        for (int i = 0; i < 16; ++i)
            if (std::fabs(palette[1].m[i] - noik[1].m[i]) > 1e-3f) differs = true;
        CHECK(differs);
    }

    // IK weight 0 == passthrough (palette equals the no-IK palette).
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        AnimationClip idle;
        idle.name = "idle";
        idle.duration = 1.0f;

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("idle", &idle);
        a.switchTo("idle");
        const int h = a.addLookAt(0, Vec3{1, 0, 0}, 3.14159f, 0.0f);
        a.setLookAtTarget(h, Vec3{0, 0, 5});
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);  // unchanged identity
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
    }

    return iron_test_result();
}
