#include "asset/AnimationStateMachine.h"
#include "asset/Animation.h"
#include "asset/CharacterAnimator.h"
#include "asset/PoseBlend.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "test_framework.h"

#include <array>

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

AnimationClip makeStillClip(const char* name) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs  = {0.0f, 1.0f};
    s.outputs = {0, 0, 0,  0, 0, 0};
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
    // Entry state is active and plays its clip.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("walk", &walk);

        AnimationStateMachine sm(&anim);
        sm.addState("walk");
        sm.setEntryState("walk");
        CHECK(sm.currentState() == "walk");

        sm.update(0.5f);
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);
    }

    // FloatAbove transition fires when the parameter crosses the threshold.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip idle = makeStillClip("idle");
        const AnimationClip run  = makeTranslateClip("run");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("idle", &idle);
        anim.setClipForState("run",  &run);

        AnimationStateMachine sm(&anim);
        sm.addState("idle");
        sm.addState("run");
        sm.setEntryState("idle");
        const int t = sm.addTransition("idle", "run", 0.0f);
        sm.whenFloatAbove(t, "speed", 2.5f);

        sm.setFloat("speed", 1.0f);
        sm.update(0.1f);
        CHECK(sm.currentState() == "idle");

        sm.setFloat("speed", 3.0f);
        sm.update(0.5f);
        CHECK(sm.currentState() == "run");
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);
    }

    // BoolEquals transition.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip ground = makeStillClip("ground");
        const AnimationClip air    = makeStillClip("air");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("ground", &ground);
        anim.setClipForState("air", &air);

        AnimationStateMachine sm(&anim);
        sm.addState("ground");
        sm.addState("air");
        sm.setEntryState("ground");
        const int t = sm.addTransition("ground", "air", 0.0f);
        sm.whenBool(t, "grounded", false);

        sm.setBool("grounded", true);
        sm.update(0.1f);
        CHECK(sm.currentState() == "ground");
        sm.setBool("grounded", false);
        sm.update(0.1f);
        CHECK(sm.currentState() == "air");
    }

    // Bound blend parameter drives the active state's blend space.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip still = makeStillClip("still");
        const AnimationClip walk  = makeTranslateClip("walk");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        BlendSpace1D loco;
        loco.add(0.0f, &still);
        loco.add(1.0f, &walk);
        anim.setBlendSpaceForState("loco", std::move(loco));

        AnimationStateMachine sm(&anim);
        sm.addState("loco");
        sm.bindBlendParam("loco", "speed");
        sm.setEntryState("loco");

        sm.setFloat("speed", 0.5f);
        sm.update(0.5f);
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }

    return iron_test_result();
}
