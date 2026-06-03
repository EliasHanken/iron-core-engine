#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace iron {

// Generates `n` view-space hemisphere sample points for SSAO: x,y in [-1,1], z in
// [0,1], each scaled to lie within the unit hemisphere, with length biased toward
// the origin (lerp(0.1, 1.0, t*t)) so more samples cluster near the fragment.
// Uses a deterministic LCG so the kernel is stable across runs and matches tests.
// Keep the hemisphere/scale convention in lockstep with the SSAO GLSL kernel use.
inline std::vector<Vec3> generateSsaoKernel(std::uint32_t n) {
    std::uint32_t state = 0x9e3779b9u;  // fixed seed → deterministic
    auto rnd01 = [&state]() {
        state = state * 1664525u + 1013904223u;       // Numerical Recipes LCG
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);  // [0,1)
    };
    std::vector<Vec3> kernel(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        Vec3 s{rnd01() * 2.0f - 1.0f, rnd01() * 2.0f - 1.0f, rnd01()};  // hemisphere dir
        const float len = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
        if (len > 1e-6f) { s.x /= len; s.y /= len; s.z /= len; }        // unit vector
        const float r = rnd01();                                        // random radius
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float scale = 0.1f + 0.9f * t * t;                        // origin-weighted
        const float m = r * scale;
        kernel[i] = Vec3{s.x * m, s.y * m, s.z * m};
    }
    return kernel;
}

// CPU port of the SSAO GLSL range-check / occlusion contribution for one sample.
// View space has the camera looking down -z (front = more negative). A sample is
// occluded when the actual surface found at its projected location is CLOSER to the
// camera (greater z) than the sample point, by more than `bias`. The contribution is
// weighted by a smoothstep range check so distant occluders fade out.
// Keep in lockstep with the GLSL.
inline float ssaoSampleOcclusion(float fragZ, float sampleZ, float surfaceZ,
                                 float radius, float bias) {
    float x = radius / std::max(std::abs(fragZ - surfaceZ), 1e-4f);
    x = std::clamp(x, 0.0f, 1.0f);
    const float rangeCheck = x * x * (3.0f - 2.0f * x);  // smoothstep(0,1,x)
    return (surfaceZ >= sampleZ + bias) ? rangeCheck : 0.0f;
}

}  // namespace iron
