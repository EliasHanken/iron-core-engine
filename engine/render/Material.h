#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

namespace iron {

// Per-surface material: how a draw should sample its texture, glow, reflect.
// Embedded by value on DrawCall. Defaults match the previous bare-DrawCall
// defaults (texture invalid, no glow, matte, no planar reflection, no
// UV tiling change).
struct Material {
    TextureHandle texture = kInvalidHandle;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
    bool useReflectionPlane = false;
    float uvScale = 1.0f;  // multiplies sampled UV; >1 = tile more times
    TextureHandle normalMap = kInvalidHandle;
    TextureHandle specularMap = kInvalidHandle;
    float specPower = 32.0f;
};

} // namespace iron
