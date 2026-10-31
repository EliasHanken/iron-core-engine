#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace iron {

// A world-space box that a 3D grid covers.
struct VolumeBounds {
    Vec3 min;
    Vec3 max;
};

struct VolumeSize {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
};

// Maps a world position into the box: 0..1 on each axis when inside.
inline Vec3 worldToVolume(const VolumeBounds& b, Vec3 world) {
    return {(world.x - b.min.x) / (b.max.x - b.min.x),
            (world.y - b.min.y) / (b.max.y - b.min.y),
            (world.z - b.min.z) / (b.max.z - b.min.z)};
}

// Metres per voxel on each axis.
inline Vec3 volumeVoxelSize(const VolumeBounds& b, VolumeSize s) {
    return {(b.max.x - b.min.x) / static_cast<float>(s.x),
            (b.max.y - b.min.y) / static_cast<float>(s.y),
            (b.max.z - b.min.z) / static_cast<float>(s.z)};
}

// Where a ray overlaps the box. t0 clamps to 0 when the origin is inside. Returns
// false when the ray misses or the box is entirely behind the origin.
inline bool rayVolumeInterval(const VolumeBounds& b, Vec3 origin, Vec3 dir, float& t0, float& t1) {
    t0 = 0.0f;
    t1 = std::numeric_limits<float>::max();

    const auto slab = [&](float o, float d, float lo, float hi) {
        if (d == 0.0f) return o >= lo && o <= hi;
        float a = (lo - o) / d;
        float b2 = (hi - o) / d;
        if (a > b2) std::swap(a, b2);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b2);
        return t0 <= t1;
    };

    return slab(origin.x, dir.x, b.min.x, b.max.x) &&
           slab(origin.y, dir.y, b.min.y, b.max.y) &&
           slab(origin.z, dir.z, b.min.z, b.max.z);
}

}  // namespace iron
