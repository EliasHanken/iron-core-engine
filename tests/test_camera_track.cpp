#include "render/CameraTrack.h"

#include "test_framework.h"

using namespace iron;

static void passes_through_its_keys_and_holds_the_ends() {
    const CameraTrack track({{2.0f, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
                             {0.0f, {-10.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
                             {5.0f, {4.0f, 2.0f, 0.0f}, {1.0f, 0.0f, -1.0f}}});
    CHECK_NEAR(track.duration(), 5.0f);
    Vec3 eye, target;
    track.sample(-1.0f, eye, target);
    CHECK_NEAR(eye.x, -10.0f);
    track.sample(2.0f, eye, target);  // Keys in any order: sorted.
    CHECK_NEAR(eye.x, 0.0f);
    track.sample(9.0f, eye, target);
    CHECK_NEAR(eye.x, 4.0f);
    CHECK_NEAR(target.x, 1.0f);
    // Between keys, between them, and moving the right way.
    track.sample(1.0f, eye, target);
    CHECK(eye.x > -10.0f && eye.x < 0.0f);
    track.sample(3.5f, eye, target);
    CHECK(eye.x > 0.0f && eye.x < 4.0f && eye.y > 1.0f && eye.y < 2.0f);
}

int main() {
    passes_through_its_keys_and_holds_the_ends();
    return iron_test_result();
}
