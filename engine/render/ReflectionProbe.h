#pragma once

#include "math/Vec.h"
#include "math/Mat4.h"
#include "math/Transform.h"   // lookAt / perspective
#include "render/Handles.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace iron {

// Authorable probe component (serialized + edited). The owning entity's
// transform supplies the world center; halfExtents define the box bounds.
struct ReflectionProbeDef {
    Vec3  halfExtents{5.0f, 5.0f, 5.0f};
    int   faceSize   = 128;   // cube face resolution (128 default, 256 hero)
    float intensity  = 1.0f;  // scales the probe's contribution (reserved)
};

// Runtime probe handed to the renderer: world-space box + baked prefiltered cube.
struct GpuReflectionProbe {
    Vec3          boxMin;
    Vec3          boxMax;
    Vec3          center;
    CubemapHandle prefiltered = kInvalidHandle;
};

inline bool boxContains(const GpuReflectionProbe& p, Vec3 pos) {
    return pos.x >= p.boxMin.x && pos.x <= p.boxMax.x &&
           pos.y >= p.boxMin.y && pos.y <= p.boxMax.y &&
           pos.z >= p.boxMin.z && pos.z <= p.boxMax.z;
}

// Returns the index of the nearest probe (by center distance) whose box
// contains `pos`, or -1 if none contain it (caller falls back to skybox IBL).
inline int nearestProbeContaining(const std::vector<GpuReflectionProbe>& probes, Vec3 pos) {
    int best = -1;
    float bestDist2 = 0.0f;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        if (!boxContains(probes[i], pos)) continue;
        const Vec3 d{pos.x - probes[i].center.x, pos.y - probes[i].center.y,
                     pos.z - probes[i].center.z};
        const float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (best == -1 || dist2 < bestDist2) { best = static_cast<int>(i); bestDist2 = dist2; }
    }
    return best;
}

// Box-projected cubemap correction (standard parallax correction). Given a
// reflection direction `R` and the fragment world position, intersect the
// reflection ray with the probe AABB and return the direction from the probe
// center to the hit point.
inline Vec3 boxProjectReflection(Vec3 R, Vec3 worldPos, const GpuReflectionProbe& p) {
    // For each axis compute the t at which the ray exits the box on the positive
    // side of R. Axes where R is zero are parallel to the slab and never hit —
    // use +infinity so they never constrain the minimum.
    const float kInf = std::numeric_limits<float>::infinity();
    auto axisT = [&](float r, float posMin, float posMax, float origin) -> float {
        if (r == 0.0f) return kInf;
        return ((r > 0 ? posMax : posMin) - origin) / r;
    };
    const float tx = axisT(R.x, p.boxMin.x, p.boxMax.x, worldPos.x);
    const float ty = axisT(R.y, p.boxMin.y, p.boxMax.y, worldPos.y);
    const float tz = axisT(R.z, p.boxMin.z, p.boxMax.z, worldPos.z);
    const float t = std::min(tx, std::min(ty, tz));
    const Vec3 hit{worldPos.x + R.x * t, worldPos.y + R.y * t, worldPos.z + R.z * t};
    return Vec3{hit.x - p.center.x, hit.y - p.center.y, hit.z - p.center.z};
}

// View matrix looking from `position` down cube face `face` (0..5 =
// +X,-X,+Y,-Y,+Z,-Z), matching ProceduralSky / Ibl.h face convention.
inline Mat4 cubeFaceView(Vec3 position, int face) {
    Vec3 fwd;
    Vec3 up;
    switch (face) {
        case 0: fwd = { 1,  0,  0}; up = {0, -1,  0}; break;  // +X
        case 1: fwd = {-1,  0,  0}; up = {0, -1,  0}; break;  // -X
        case 2: fwd = { 0,  1,  0}; up = {0,  0,  1}; break;  // +Y
        case 3: fwd = { 0, -1,  0}; up = {0,  0, -1}; break;  // -Y
        case 4: fwd = { 0,  0,  1}; up = {0, -1,  0}; break;  // +Z
        default: fwd = { 0, 0, -1}; up = {0, -1,  0}; break;  // -Z
    }
    const Vec3 target{position.x + fwd.x, position.y + fwd.y, position.z + fwd.z};
    return lookAt(position, target, up);
}

}  // namespace iron
