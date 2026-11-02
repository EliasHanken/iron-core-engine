#include "render/ShadowCascades.h"

#include "math/Transform.h"

#include <algorithm>
#include <cmath>

namespace iron {

namespace {

Vec3 transformPoint(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

}  // namespace

std::array<float, ShadowCascades::kCount> cascadeEnds(const ShadowCascadeSettings& s) {
    std::array<float, ShadowCascades::kCount> ends{};
    const float n = std::max(s.near, 1e-3f);
    const float f = std::max(s.distance, n * 2.0f);
    for (int i = 0; i < ShadowCascades::kCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(ShadowCascades::kCount);
        const float logarithmic = n * std::pow(f / n, p);
        const float even = n + (f - n) * p;
        ends[static_cast<std::size_t>(i)] = s.split * logarithmic + (1.0f - s.split) * even;
    }
    ends.back() = f;
    return ends;
}

ShadowCascades computeShadowCascades(Vec3 sunDirection, const Mat4& cameraToWorld, float fovY, float aspect,
                                     const ShadowCascadeSettings& settings) {
    ShadowCascades out;
    out.ends = cascadeEnds(settings);
    const Vec3 dir = normalize(sunDirection);
    const Vec3 up = std::fabs(dir.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    // The light's orientation alone, for snapping in its plane.
    const Mat4 lightRotation = lookAt({0.0f, 0.0f, 0.0f}, dir, up);
    const Mat4 lightRotationInverse = inverse(lightRotation);

    const Vec3 eye = transformPoint(cameraToWorld, {0.0f, 0.0f, 0.0f});
    const Vec3 forward = normalize(transformPoint(cameraToWorld, {0.0f, 0.0f, -1.0f}) - eye);
    const float tanY = std::tan(fovY * 0.5f);
    const float tanX = tanY * aspect;

    float start = std::max(settings.near, 1e-3f);
    for (int c = 0; c < ShadowCascades::kCount; ++c) {
        const float end = out.ends[static_cast<std::size_t>(c)];
        // The slice's bounding sphere. For a symmetric frustum the smallest one is
        // centred on the view axis, at a depth set by the two end rectangles' half-
        // diagonals; past the far corners it's just the far rectangle's centre.
        const float k = tanX * tanX + tanY * tanY;  // Half-diagonal squared per unit depth.
        float centreDepth = 0.5f * (start + end) * (1.0f + k);
        centreDepth = std::min(centreDepth, end);
        const float farRadius2 = (end - centreDepth) * (end - centreDepth) + end * end * k;
        const float nearRadius2 = (centreDepth - start) * (centreDepth - start) + start * start * k;
        float radius = std::sqrt(std::max(farRadius2, nearRadius2));
        // A little rounding up, so tiny changes in the fit don't change the scale.
        radius = std::ceil(radius * 16.0f) / 16.0f;
        Vec3 centre = eye + forward * centreDepth;

        // Snap the centre to whole texels in the light's plane, so the map's grid
        // stays put in the world as the camera moves.
        const float texel = 2.0f * radius / static_cast<float>(std::max(settings.resolution, 1));
        Vec3 inLight = transformPoint(lightRotation, centre);
        inLight.x = std::floor(inLight.x / texel) * texel;
        inLight.y = std::floor(inLight.y / texel) * texel;
        centre = transformPoint(lightRotationInverse, inLight);

        const float back = radius + settings.casterReach;
        const Mat4 view = lookAt(centre - dir * back, centre, up);
        const Mat4 proj = orthographic(-radius, radius, -radius, radius, 0.0f, back + radius);
        Mat4 toVulkanDepth = Mat4::identity();
        toVulkanDepth.at(2, 2) = 0.5f;
        toVulkanDepth.at(2, 3) = 0.5f;
        out.viewProj[static_cast<std::size_t>(c)] = toVulkanDepth * proj * view;
        out.radii[static_cast<std::size_t>(c)] = radius;
        start = end;
    }
    return out;
}

}  // namespace iron
