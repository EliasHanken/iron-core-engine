#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the cube-face direction + equirect mapping in
// equirectToCube.comp. Keep these in lockstep with the GLSL.
inline constexpr float kIblPi = 3.14159265358979323846f;

// Reconstructs the world-space direction for a texel on cube `face`
// (0..5 = +X,-X,+Y,-Y,+Z,-Z) at face coordinates u, v in [-1, 1].
// Matches ProceduralSky.cpp's face convention so HDR cubemaps share the
// orientation of LDR ones.
inline Vec3 cubeFaceDirection(int face, float u, float v) {
    Vec3 d{};
    switch (face) {
        case 0: d = { 1.0f, -v,   -u};   break;  // +X
        case 1: d = {-1.0f, -v,    u};   break;  // -X
        case 2: d = { u,    1.0f,  v};   break;  // +Y
        case 3: d = { u,   -1.0f, -v};   break;  // -Y
        case 4: d = { u,   -v,    1.0f}; break;  // +Z
        default: d = {-u,  -v,   -1.0f}; break;  // -Z
    }
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return Vec3{d.x / len, d.y / len, d.z / len};
}

// Maps a unit direction to equirectangular UV in [0, 1]. u wraps yaw
// (atan2(z, x)); v maps pitch via asin(y) so the horizon sits at v = 0.5
// and straight up at v = 0.0 (an equirect map stores the zenith in its top
// row, which the sampler reads at v = 0).
inline Vec2 directionToEquirectUv(Vec3 dir) {
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const Vec3 d{dir.x / len, dir.y / len, dir.z / len};
    const float u = std::atan2(d.z, d.x) / (2.0f * kIblPi) + 0.5f;
    const float v = 0.5f - std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kIblPi;
    return Vec2{u, v};
}

// CPU port of the normalization in irradianceConvolve.comp. Runs the same
// cosine-weighted hemisphere loop for a UNIFORM environment of radiance L;
// the PI/nrSamples normalization makes the result == L (energy conservation).
// Used to lock the GLSL normalization constant in tests.
inline Vec3 convolveConstantIrradiance(Vec3 L, float sampleDelta = 0.025f) {
    float sum = 0.0f;
    float nrSamples = 0.0f;
    for (float phi = 0.0f; phi < 2.0f * kIblPi; phi += sampleDelta) {
        for (float theta = 0.0f; theta < 0.5f * kIblPi; theta += sampleDelta) {
            sum += std::cos(theta) * std::sin(theta);
            nrSamples += 1.0f;
        }
    }
    const float scale = kIblPi * sum / nrSamples;  // ~= 1.0
    return Vec3{L.x * scale, L.y * scale, L.z * scale};
}

}  // namespace iron
