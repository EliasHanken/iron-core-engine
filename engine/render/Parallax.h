#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the parallax-occlusion ray-march in StandardLitShader.h.
// Keep in lockstep with the GLSL. Height convention: sampled value in [0,1]
// is HEIGHT (white=peak); depth = 1 - height. viewTS is the tangent-space
// direction from the surface toward the camera (normalized). Returns the
// parallax-offset UV.
inline constexpr int kPomMinLayers = 8;
inline constexpr int kPomMaxLayers = 32;

// Adaptive layer count: more layers at grazing angles (small viewTS.z).
inline float parallaxLayerCount(Vec3 viewTS) {
    const float t = std::fabs(viewTS.z);  // 1 head-on, ~0 grazing
    return static_cast<float>(kPomMaxLayers)
         + (static_cast<float>(kPomMinLayers) - static_cast<float>(kPomMaxLayers)) * t;
}

// sampleHeight: callable Vec2 -> float height in [0,1].
template <typename SampleHeight>
inline Vec2 parallaxOcclusionOffset(SampleHeight sampleHeight, Vec2 uv, Vec3 viewTS, float heightScale) {
    const float numLayers = parallaxLayerCount(viewTS);
    const float layerDepth = 1.0f / numLayers;
    const float vz = (std::fabs(viewTS.z) < 1e-3f) ? 1e-3f : viewTS.z;
    const Vec2 P{viewTS.x / vz * heightScale, viewTS.y / vz * heightScale};
    const Vec2 deltaUV{P.x / numLayers, P.y / numLayers};

    float currentLayerDepth = 0.0f;
    Vec2 curUV = uv;
    float curDepth = 1.0f - sampleHeight(curUV);
    int guard = 0;
    while (currentLayerDepth < curDepth && guard < kPomMaxLayers + 1) {
        curUV = Vec2{curUV.x - deltaUV.x, curUV.y - deltaUV.y};
        curDepth = 1.0f - sampleHeight(curUV);
        currentLayerDepth += layerDepth;
        ++guard;
    }

    // Occlusion interpolation between the last two layers.
    const Vec2 prevUV{curUV.x + deltaUV.x, curUV.y + deltaUV.y};
    const float afterDepth = curDepth - currentLayerDepth;
    const float beforeDepth = (1.0f - sampleHeight(prevUV)) - (currentLayerDepth - layerDepth);
    const float denom = afterDepth - beforeDepth;
    const float weight = (std::fabs(denom) < 1e-6f) ? 0.0f
                       : std::clamp(afterDepth / denom, 0.0f, 1.0f);
    return Vec2{curUV.x * (1.0f - weight) + prevUV.x * weight,
                curUV.y * (1.0f - weight) + prevUV.y * weight};
}

}  // namespace iron
