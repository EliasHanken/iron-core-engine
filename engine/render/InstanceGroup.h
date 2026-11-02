#pragma once

#include "math/Aabb.h"
#include "render/Handles.h"
#include "render/Material.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iron {

// Many copies of one mesh that live on the GPU: placed once, then culled, given a
// level of detail and drawn every frame without the game submitting them, like an
// instanced static mesh in Unreal. A model with several materials is one group per
// material, sharing the same transforms.
using InstanceGroupHandle = std::uint32_t;
constexpr InstanceGroupHandle kInvalidInstanceGroup = 0;

struct InstanceGroupDesc {
    // Finest first, at most four, all built from the same vertices (see generateLods).
    std::vector<MeshHandle> lods;
    // An object drops to the next level when it's fewer pixels tall than these.
    std::array<float, 3> lodPixels{160.0f, 60.0f, 20.0f};
    // Smaller than this on screen, it isn't drawn at all.
    float minPixels = 1.5f;
    // From Renderer::createInstancedLitShader, or another instanced shader.
    ShaderHandle shader = kInvalidHandle;
    Material material;
    Aabb bounds{};  // In the mesh's own space.
    bool castShadows = true;
};

}  // namespace iron
