#include "math/Transform.h"
#include "render/AntiAliasing.h"

#include "test_framework.h"

#include <cmath>

using namespace iron;

static void halton_fills_the_interval() {
    CHECK_NEAR(halton(1, 2), 0.5f);
    CHECK_NEAR(halton(2, 2), 0.25f);
    CHECK_NEAR(halton(3, 2), 0.75f);
    CHECK_NEAR(halton(1, 3), 1.0f / 3.0f);
    CHECK_NEAR(halton(2, 3), 2.0f / 3.0f);
    CHECK_NEAR(halton(3, 3), 1.0f / 9.0f);
}

static void jitter_stays_within_half_a_pixel_and_averages_out() {
    float sx = 0.0f, sy = 0.0f;
    for (std::uint32_t f = 0; f < 8; ++f) {
        const Vec2 j = temporalJitter(f);
        CHECK(j.x >= -0.5f && j.x < 0.5f);
        CHECK(j.y >= -0.5f && j.y < 0.5f);
        sx += j.x;
        sy += j.y;
    }
    CHECK(std::abs(sx / 8.0f) < 0.07f);
    CHECK(std::abs(sy / 8.0f) < 0.07f);
    // It cycles.
    CHECK_NEAR(temporalJitter(3).x, temporalJitter(11).x);
}

static void jitter_moves_the_image_by_whole_pixels_given() {
    const Mat4 p = perspective(1.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    const Mat4 j = jitterMatrix({1.0f, -1.0f}, 1920.0f, 1080.0f);
    const Vec4 point{0.3f, -0.2f, -5.0f, 1.0f};
    const Vec4 a = p * point;
    const Vec4 b = (j * p) * point;
    // One pixel is 2/width of NDC across, 2/height up.
    CHECK_NEAR(b.x / b.w - a.x / a.w, 2.0f / 1920.0f);
    CHECK_NEAR(b.y / b.w - a.y / a.w, -2.0f / 1080.0f);
    CHECK_NEAR(b.z / b.w, a.z / a.w);
}

int main() {
    halton_fills_the_interval();
    jitter_stays_within_half_a_pixel_and_averages_out();
    jitter_moves_the_image_by_whole_pixels_given();
    return iron_test_result();
}
