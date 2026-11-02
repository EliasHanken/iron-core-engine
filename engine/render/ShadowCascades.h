#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <array>

namespace iron {

// Cascaded shadow maps for the sun: the camera's view out to `distance` is cut
// into slices, near ones small and sharp, far ones wide, each with its own shadow
// map. Each slice is wrapped in a bounding sphere, so a cascade keeps its size as
// the camera turns, and its origin snaps to whole shadow-map texels, so shadow
// edges don't crawl as the camera moves.
struct ShadowCascades {
    static constexpr int kCount = 4;
    // Light view-projection per cascade, with depth mapped to Vulkan's 0..1.
    std::array<Mat4, kCount> viewProj{};
    // Distance from the camera at which each cascade ends.
    std::array<float, kCount> ends{};
    // Each cascade's bounding-sphere radius: its shadow map spans twice this.
    std::array<float, kCount> radii{};
};

struct ShadowCascadeSettings {
    float distance = 100.0f;   // How far shadows reach.
    float near = 0.1f;         // Where the first slice starts.
    float split = 0.75f;       // 0 even slices, 1 logarithmic: how much nearer slices crowd.
    float casterReach = 60.0f; // How far towards the sun casters may stand outside a slice.
    int   resolution = 2048;   // Each cascade's shadow map, for texel snapping.
};

// Where each slice ends: a blend of even and logarithmic spacing (the "practical
// split scheme").
std::array<float, ShadowCascades::kCount> cascadeEnds(const ShadowCascadeSettings& s);

// The cascades for a camera at `cameraToWorld` (looking down its -z) with vertical
// field of view `fovY` and `aspect`, lit by a sun shining along `sunDirection`.
ShadowCascades computeShadowCascades(Vec3 sunDirection, const Mat4& cameraToWorld, float fovY, float aspect,
                                     const ShadowCascadeSettings& settings);

}  // namespace iron
