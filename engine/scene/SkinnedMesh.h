#pragma once

#include "asset/Skeleton.h"
#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// Skinned vertex: geometry + 4 bone influences.
// Total size: 76 bytes (11 floats + 4 uint32 + 4 floats).
struct SkinnedVertex {
    Vec3          position;
    Vec3          normal;
    Vec2          uv;
    Vec3          tangent;
    std::uint32_t joints[4];   // bone indices into Skeleton::bones
    float         weights[4];  // per-influence weights; normalized to sum 1
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};

}  // namespace iron
