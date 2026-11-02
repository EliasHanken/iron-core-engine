#include "asset/SkeletalAnimator.h"

#include "test_framework.h"

using namespace iron;

static Skeleton oneBone() {
    Skeleton s;
    Bone b;
    b.parentIndex = -1;
    b.inverseBindMatrix = Mat4::identity();
    b.localBindTransform = Mat4::identity();
    b.name = "root";
    s.bones.push_back(b);
    return s;
}

// Moves the bone along x from `from` to `to` over `length` seconds.
static AnimationClip slide(const char* name, float from, float to, float length = 1.0f) {
    AnimationClip c;
    c.name = name;
    c.duration = length;
    AnimationSampler s;
    s.inputs = {0.0f, length};
    s.outputs = {from, 0.0f, 0.0f, to, 0.0f, 0.0f};
    c.samplers.push_back(s);
    c.channels.push_back({0, AnimationPath::Translation, 0});
    return c;
}

static float x(const SkeletalAnimator& a) { return a.pose().bones[0].translation.x; }

static void base_weights_average() {
    const Skeleton sk = oneBone();
    const AnimationClip a = slide("a", 0.0f, 0.0f);
    const AnimationClip b = slide("b", 2.0f, 2.0f);
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.setBaseWeight(&a, 1.0f);
    anim.setBaseWeight(&b, 1.0f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 1.0f);
    anim.setBaseWeight(&b, 3.0f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 1.5f);
    anim.setBaseWeight(&b, 0.0f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 0.0f);
}

static void one_shots_fade_in_and_out() {
    const Skeleton sk = oneBone();
    const AnimationClip base = slide("base", 0.0f, 0.0f);
    const AnimationClip shot = slide("shot", 2.0f, 2.0f, 2.0f);
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.setBaseWeight(&base, 1.0f);
    anim.playOneShot(&shot, 0.5f, 0.5f);
    anim.update(0.25f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 1.0f);  // Halfway in.
    anim.update(1.0f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 2.0f);  // Fully in.
    anim.update(0.5f);          // 0.25 s left of a 0.5 s fade out.
    anim.evaluate();
    CHECK_NEAR(x(anim), 1.0f);
    anim.update(0.5f);
    anim.evaluate();
    CHECK(!anim.oneShotPlaying());
    CHECK_NEAR(x(anim), 0.0f);
}

static void a_new_one_shot_fades_the_old_out() {
    const Skeleton sk = oneBone();
    const AnimationClip first = slide("first", 2.0f, 2.0f, 2.0f);
    const AnimationClip second = slide("second", 4.0f, 4.0f, 2.0f);
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.playOneShot(&first, 0.0f, 0.0f);
    anim.update(0.5f);
    anim.playOneShot(&second, 0.0f, 0.0f);
    CHECK(anim.oneShot() == &second);
    anim.update(0.1f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 4.0f);
}

static void additives_add_their_change() {
    const Skeleton sk = oneBone();
    const AnimationClip base = slide("base", 2.0f, 2.0f);
    const AnimationClip kick = slide("kick", 5.0f, 6.0f, 1.0f);  // Moves +1 over its length.
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.setBaseWeight(&base, 1.0f);
    anim.playAdditive(&kick);
    anim.update(0.5f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 2.5f);  // The base, plus half the kick's change; not its 5.
    anim.update(1.0f);
    anim.evaluate();
    CHECK_NEAR(x(anim), 2.0f);  // Finished and gone.
}

static void events_fire_once_as_crossed() {
    const Skeleton sk = oneBone();
    AnimationClip reload = slide("reload", 0.0f, 0.0f, 1.0f);
    reload.looping = false;
    reload.events = {{0.0f, "start"}, {0.05f, "mag_out"}, {0.5f, "mag_in"}};
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.playOneShot(&reload, 0.1f, 0.1f);
    const auto& first = anim.update(0.1f);
    CHECK(first.size() == 2);
    CHECK(first.size() == 2 && first[0].name == "start" && first[1].name == "mag_out");
    CHECK(anim.update(0.1f).empty());
    const auto& later = anim.update(0.5f);
    CHECK(later.size() == 1 && later[0].name == "mag_in");
}

static void looping_events_fire_every_lap() {
    const Skeleton sk = oneBone();
    AnimationClip walk = slide("walk", 0.0f, 0.0f, 1.0f);
    walk.events = {{0.9f, "step"}};
    SkeletalAnimator anim;
    anim.setSkeleton(&sk);
    anim.setBaseWeight(&walk, 1.0f);
    CHECK(anim.update(0.95f).size() == 1);
    CHECK(anim.update(0.1f).empty());   // Wrapped to 0.05.
    CHECK(anim.update(0.9f).size() == 1);
}

int main() {
    base_weights_average();
    one_shots_fade_in_and_out();
    a_new_one_shot_fades_the_old_out();
    additives_add_their_change();
    events_fire_once_as_crossed();
    looping_events_fire_every_lap();
    return iron_test_result();
}
