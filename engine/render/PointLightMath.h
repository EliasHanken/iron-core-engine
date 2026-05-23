#pragma once

#include "math/Vec.h"
#include "render/Light.h"

#include <cmath>

namespace iron {

// CPU mirror of the shader's per-fragment point-light contribution. Kept
// in lockstep with the GLSL in the lit fragment shader so we can unit-test
// the math without a GL context. Any change here MUST be mirrored in the
// shader (and vice versa).
inline Vec3 pointLightContribution(const PointLight& light,
                                   Vec3 fragPos,
                                   Vec3 normal) {
    Vec3 toLight{light.position.x - fragPos.x,
                 light.position.y - fragPos.y,
                 light.position.z - fragPos.z};
    float dist = std::sqrt(toLight.x * toLight.x +
                           toLight.y * toLight.y +
                           toLight.z * toLight.z);

    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). Matches the shader's `dist < 0.0001 || dist >= range`.
    if (dist < 0.0001f || dist >= light.range) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    Vec3 L{toLight.x / dist, toLight.y / dist, toLight.z / dist};
    float lambert = normal.x * L.x + normal.y * L.y + normal.z * L.z;
    if (lambert < 0.0f) lambert = 0.0f;

    // 1 - smoothstep(0, range, dist): full at dist=0, zero at dist=range.
    float t = dist / light.range;          // in [0, 1)
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float smoothed = t * t * (3.0f - 2.0f * t); // smoothstep
    float falloff = 1.0f - smoothed;

    float scale = light.intensity * lambert * falloff;
    return Vec3{light.color.x * scale,
                light.color.y * scale,
                light.color.z * scale};
}

} // namespace iron
