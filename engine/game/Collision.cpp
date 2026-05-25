#include "game/Collision.h"

#include <algorithm>

namespace iron {

bool sphereOverlapAabb(const Vec3& center, float radius, const Aabb& box) {
    const float cx = std::clamp(center.x, box.min.x, box.max.x);
    const float cy = std::clamp(center.y, box.min.y, box.max.y);
    const float cz = std::clamp(center.z, box.min.z, box.max.z);
    const float dx = center.x - cx;
    const float dy = center.y - cy;
    const float dz = center.z - cz;
    return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
}

}  // namespace iron
