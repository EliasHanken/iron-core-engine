#include "render/Culling.h"

#include <algorithm>
#include <cmath>

namespace iron {

namespace {

Vec4 row(const Mat4& m, int r) { return {m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; }

Vec4 add(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
Vec4 sub(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }

Vec4 normalized(Vec4 p) {
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return len > 0.0f ? Vec4{p.x / len, p.y / len, p.z / len, p.w / len} : p;
}

}  // namespace

Frustum frustumFromViewProjection(const Mat4& m) {
    const Vec4 r0 = row(m, 0), r1 = row(m, 1), r2 = row(m, 2), r3 = row(m, 3);
    return {normalized(add(r3, r0)), normalized(sub(r3, r0)), normalized(add(r3, r1)),
            normalized(sub(r3, r1)), normalized(r2), normalized(sub(r3, r2))};
}

bool sphereInFrustum(const Frustum& frustum, Vec3 c, float r) {
    for (const Vec4& p : frustum) {
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    }
    return true;
}

float projectedPixels(Vec3 center, float radius, Vec3 eye, float pixelsPerUnitAtOne) {
    const float distance = std::max(length(center - eye), 1e-3f);
    return 2.0f * radius * pixelsPerUnitAtOne / distance;
}

std::uint32_t selectLod(float pixels, std::span<const float> thresholds, std::uint32_t levels) {
    std::uint32_t lod = 0;
    while (lod + 1 < levels && lod < thresholds.size() && pixels < thresholds[lod]) ++lod;
    return lod;
}

}  // namespace iron
