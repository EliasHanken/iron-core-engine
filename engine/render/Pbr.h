#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the Cook-Torrance GGX BRDF helpers in StandardLitShader.h.
// Keep these in lockstep with the GLSL `D`/`G`/`F`/`F0` functions.
inline constexpr float kPi = 3.14159265358979323846f;

// Base reflectance: 0.04 for dielectrics, albedo for metals.
inline Vec3 f0For(Vec3 albedo, float metallic) {
    return Vec3{0.04f + (albedo.x - 0.04f) * metallic,
               0.04f + (albedo.y - 0.04f) * metallic,
               0.04f + (albedo.z - 0.04f) * metallic};
}

// Fresnel-Schlick: reflectance at angle whose cosine is cosTheta.
inline Vec3 fresnelSchlick(float cosTheta, Vec3 f0) {
    float m = std::clamp(1.0f - cosTheta, 0.0f, 1.0f);
    float m5 = m * m * m * m * m;
    return Vec3{f0.x + (1.0f - f0.x) * m5,
               f0.y + (1.0f - f0.y) * m5,
               f0.z + (1.0f - f0.z) * m5};
}

// Trowbridge-Reitz GGX normal distribution.
inline float distributionGGX(float nDotH, float roughness) {
    roughness = std::max(roughness, 1e-3f);  // guard 0/0 (the GLSL pre-clamps to >=0.04)
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (nDotH * nDotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Smith geometry (Schlick-GGX, direct-lighting k).
inline float geometrySmith(float nDotV, float nDotL, float roughness) {
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    auto g1 = [k](float nd) { return nd / (nd * (1.0f - k) + k); };
    return g1(nDotV) * g1(nDotL);
}

}  // namespace iron
