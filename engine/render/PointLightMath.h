#pragma once

#include "math/Vec.h"
#include "render/Light.h"

namespace iron {

// CPU mirror of the shader's per-fragment point-light contribution. Kept
// in lockstep with the GLSL in the lit fragment shader so we can unit-test
// the math without a GL context. Any change here MUST be mirrored in the
// shader (and vice versa).
inline Vec3 pointLightContribution(const PointLight& light,
                                   Vec3 fragPos,
                                   Vec3 normal) {
    // Anything closer than 0.1 mm is treated as co-located with the
    // light; below that we'd divide by ~zero and produce NaN.
    constexpr float kMinLightDist = 0.0001f;

    Vec3 toLight = light.position - fragPos;
    float dist = length(toLight);

    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). Matches the shader's `dist < 0.0001 || dist >= range`.
    if (dist < kMinLightDist || dist >= light.range) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    // Avoid a second sqrt: we already have dist, so divide directly.
    Vec3 L = toLight * (1.0f / dist);
    float lambert = dot(normal, L);
    if (lambert < 0.0f) lambert = 0.0f;

    // 1 - smoothstep(0, range, dist): full at dist=0, zero at dist=range.
    // t is strictly in (0, 1) here: the guard above ensures dist > kMinLightDist
    // and dist < range, so t = dist/range is in (kMinLightDist/range, 1).
    float t = dist / light.range;
    float smoothed = t * t * (3.0f - 2.0f * t); // smoothstep
    float falloff = 1.0f - smoothed;

    float scale = light.intensity * lambert * falloff;
    return light.color * scale;
}

} // namespace iron
