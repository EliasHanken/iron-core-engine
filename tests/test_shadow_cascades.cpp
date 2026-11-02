#include "math/Transform.h"
#include "render/ShadowCascades.h"

#include "test_framework.h"

#include <cmath>

using namespace iron;

static Vec4 project(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x / r.w, r.y / r.w, r.z / r.w, 1.0f};
}

static Mat4 camera(Vec3 eye, float yaw) {
    const Vec3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    return inverse(lookAt(eye, eye + forward, {0.0f, 1.0f, 0.0f}));
}

static const Vec3 kSun = normalize(Vec3{-0.4f, -1.0f, -0.3f});
static constexpr float kFov = 1.0472f;  // 60 degrees.
static constexpr float kAspect = 16.0f / 9.0f;

static void slices_grow_out_to_the_distance() {
    ShadowCascadeSettings s;
    s.distance = 120.0f;
    const auto ends = cascadeEnds(s);
    CHECK(ends[0] > s.near);
    for (int i = 1; i < ShadowCascades::kCount; ++i) CHECK(ends[i] > ends[i - 1]);
    CHECK_NEAR(ends[3], 120.0f);
    CHECK(ends[0] < 120.0f / 4.0f);  // Logarithmic crowding: the first slice is short.
}

static void every_slice_fits_its_cascade() {
    ShadowCascadeSettings s;
    s.distance = 120.0f;
    const Mat4 cam = camera({3.0f, 1.7f, -5.0f}, 0.6f);
    const ShadowCascades c = computeShadowCascades(kSun, cam, kFov, kAspect, s);
    float start = s.near;
    const float tanY = std::tan(kFov * 0.5f), tanX = tanY * kAspect;
    for (int i = 0; i < ShadowCascades::kCount; ++i) {
        for (float d : {start, c.ends[i]}) {
            for (float sx : {-1.0f, 1.0f}) {
                for (float sy : {-1.0f, 1.0f}) {
                    const Vec4 viewPoint = cam * Vec4{sx * tanX * d, sy * tanY * d, -d, 1.0f};
                    const Vec4 p = project(c.viewProj[i], {viewPoint.x, viewPoint.y, viewPoint.z});
                    CHECK(std::fabs(p.x) <= 1.0f && std::fabs(p.y) <= 1.0f);
                    CHECK(p.z >= 0.0f && p.z <= 1.0f);
                }
            }
        }
        start = c.ends[i];
    }
}

static void turning_keeps_each_cascade_its_size() {
    ShadowCascadeSettings s;
    const ShadowCascades a = computeShadowCascades(kSun, camera({0, 2, 0}, 0.0f), kFov, kAspect, s);
    const ShadowCascades b = computeShadowCascades(kSun, camera({0, 2, 0}, 2.3f), kFov, kAspect, s);
    for (int i = 0; i < ShadowCascades::kCount; ++i) CHECK_NEAR(a.radii[i], b.radii[i]);
}

static void the_shadow_grid_stays_put_in_the_world() {
    // A fixed point lands on the same sub-texel position however the camera moves:
    // the map moves only in whole texels.
    ShadowCascadeSettings s;
    const Vec3 point{1.3f, 0.0f, -7.9f};
    const auto texelPhase = [&](Vec3 eye) {
        const ShadowCascades c = computeShadowCascades(kSun, camera(eye, 0.2f), kFov, kAspect, s);
        const Vec4 p = project(c.viewProj[0], point);
        const float u = (p.x * 0.5f + 0.5f) * static_cast<float>(s.resolution);
        return u - std::floor(u);
    };
    const float a = texelPhase({0.0f, 1.7f, 0.0f});
    const float b = texelPhase({0.0137f, 1.7f, 0.021f});
    const float c = texelPhase({0.31f, 1.7f, -0.17f});
    CHECK(std::fabs(a - b) < 0.02f || std::fabs(std::fabs(a - b) - 1.0f) < 0.02f);
    CHECK(std::fabs(a - c) < 0.02f || std::fabs(std::fabs(a - c) - 1.0f) < 0.02f);
}

static void casters_towards_the_sun_stay_in_range() {
    ShadowCascadeSettings s;
    const Mat4 cam = camera({0, 2, 0}, 0.0f);
    const ShadowCascades c = computeShadowCascades(kSun, cam, kFov, kAspect, s);
    // A point in the first slice, and something 40 m up the sun's ray from it.
    const Vec3 ground{0.0f, 0.0f, -3.0f};
    const Vec4 caster = project(c.viewProj[0], ground - kSun * 40.0f);
    CHECK(caster.z >= 0.0f && caster.z < project(c.viewProj[0], ground).z);
}

int main() {
    slices_grow_out_to_the_distance();
    every_slice_fits_its_cascade();
    turning_keeps_each_cascade_its_size();
    the_shadow_grid_stays_put_in_the_world();
    casters_towards_the_sun_stay_in_range();
    return iron_test_result();
}
