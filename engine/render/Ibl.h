#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// --- M46c: split-sum BRDF integration (CPU port of brdfLut.comp) ---
// Van der Corput radical inverse + Hammersley point set.
inline Vec2 hammersley(std::uint32_t i, std::uint32_t n) {
    std::uint32_t bits = i;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    const float rdi = static_cast<float>(bits) * 2.3283064365386963e-10f;  // / 2^32
    return Vec2{static_cast<float>(i) / static_cast<float>(n), rdi};
}

// GGX importance-sampled half-vector in tangent space (N = +Z).
inline Vec3 importanceSampleGGXLocal(Vec2 xi, float roughness) {
    const float a = roughness * roughness;
    const float phi = 2.0f * kIblPi * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    return Vec3{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

// Smith geometry (IBL k = a^2/2) — Schlick-GGX, one direction.
inline float geometrySchlickGgx(float nDotX, float roughness) {
    const float a = roughness;
    const float k = (a * a) / 2.0f;
    return nDotX / (nDotX * (1.0f - k) + k);
}

// Integrates the split-sum scale (.x) + bias (.y) for given NdotV + roughness.
// V is in the tangent frame with N = +Z; everything stays in that frame.
inline Vec2 integrateBrdf(float nDotV, float roughness, std::uint32_t samples) {
    nDotV = std::max(nDotV, 1e-4f);
    const Vec3 V{std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV};  // sin, 0, cos
    float A = 0.0f, B = 0.0f;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const Vec2 xi = hammersley(i, samples);
        const Vec3 H  = importanceSampleGGXLocal(xi, roughness);
        const float vDotH = std::max(V.x * H.x + V.y * H.y + V.z * H.z, 0.0f);
        // L = reflect(-V, H) = 2*(V.H)*H - V
        const Vec3 L{2.0f * vDotH * H.x - V.x, 2.0f * vDotH * H.y - V.y, 2.0f * vDotH * H.z - V.z};
        const float nDotL = std::max(L.z, 0.0f);
        const float nDotH = std::max(H.z, 0.0f);
        if (nDotL > 0.0f) {
            const float g  = geometrySchlickGgx(nDotL, roughness) * geometrySchlickGgx(nDotV, roughness);
            const float gv = (g * vDotH) / std::max(nDotH * nDotV, 1e-4f);
            const float fc = std::pow(1.0f - vDotH, 5.0f);
            A += (1.0f - fc) * gv;
            B += fc * gv;
        }
    }
    return Vec2{A / static_cast<float>(samples), B / static_cast<float>(samples)};
}

}  // namespace iron
