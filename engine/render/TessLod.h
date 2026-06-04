#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the per-edge tessellation factor in standardLitTescSource().
// Keep in lockstep with the GLSL. Given two clip-space endpoints of a patch
// edge, returns the tessellation level: clamp(ndcEdgeLength / targetEdge, 1, maxF).
// If either endpoint is behind the near plane (w <= 0), returns maxF so patches
// straddling the camera don't collapse.
inline float tessEdgeFactor(Vec4 clipA, Vec4 clipB, float targetEdge, float maxFactor) {
    if (clipA.w <= 0.0f || clipB.w <= 0.0f) return maxFactor;
    const Vec2 a{clipA.x / clipA.w, clipA.y / clipA.w};
    const Vec2 b{clipB.x / clipB.w, clipB.y / clipB.w};
    const float ndcLen = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    const float t = (targetEdge < 1e-4f) ? 1e-4f : targetEdge;
    return std::clamp(ndcLen / t, 1.0f, maxFactor);
}

}  // namespace iron
