#pragma once

// The visibility maths the GPU culls with, written once more in C++ so that it can
// be tested and used on the CPU. Keep in step with the culling shader in
// VkInstanceGroups.cpp.

#include "math/Mat4.h"
#include "math/Vec.h"

#include <array>
#include <cstdint>
#include <span>

namespace iron {

// Six planes facing inward (xyz normal, w distance): a point p is inside a plane when
// dot(xyz, p) + w >= 0. Order: left, right, bottom, top, near, far.
using Frustum = std::array<Vec4, 6>;

// The view frustum of a view-projection matrix built by iron::perspective. Its depth
// comes out in OpenGL's -1..1 and Vulkan clips below 0, so the near plane is where
// clip z reaches 0.
Frustum frustumFromViewProjection(const Mat4& viewProjection);

// False only when the whole sphere is outside one plane.
bool sphereInFrustum(const Frustum& frustum, Vec3 center, float radius);

// How tall a sphere looks, in pixels: its diameter over its distance, times the
// projection's scale (half the screen height times the projection's y focal length).
float projectedPixels(Vec3 center, float radius, Vec3 eye, float pixelsPerUnitAtOne);

// The level of detail for an object `pixels` tall: 0 while it's at least
// thresholds[0], 1 while at least thresholds[1], and so on. `levels` caps the result.
std::uint32_t selectLod(float pixels, std::span<const float> thresholds, std::uint32_t levels);

// A camera that culls without drawing: what the player's view keeps and drops,
// while another camera watches (see Renderer::setCullingCamera).
struct CullingCamera {
    Mat4 view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Vec3 eye{};
};

// How many objects a culling pass saw, and what it did with them.
struct CullStats {
    std::uint32_t instances = 0;
    std::uint32_t drawn = 0;
    std::uint32_t outsideView = 0;  // Frustum culled.
    std::uint32_t hidden = 0;       // Occlusion culled: behind something drawn last frame.
    std::uint32_t tiny = 0;         // Too small on screen to matter.
    std::array<std::uint32_t, 4> perLod{};
};

}  // namespace iron
