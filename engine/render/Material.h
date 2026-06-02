#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

namespace iron {

// Per-surface material: how a draw should sample its texture, glow, reflect.
// Embedded by value on DrawCall. Defaults match the previous bare-DrawCall
// defaults (texture invalid, no glow, matte, no planar reflection, no
// UV tiling change).
struct Material {
    TextureHandle texture = kInvalidHandle;       // albedo (sRGB)
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
    bool useReflectionPlane = false;
    float uvScale = 1.0f;  // multiplies sampled UV; >1 = tile more times
    TextureHandle normalMap = kInvalidHandle;     // linear
    TextureHandle metallicRoughnessMap = kInvalidHandle;  // .g = roughness, .b = metallic (linear, glTF)
    TextureHandle aoMap = kInvalidHandle;                 // .r = ambient occlusion (linear)
    TextureHandle emissiveMap = kInvalidHandle;           // sRGB; multiplies `emissive`
    Vec3 baseColorFactor{1.0f, 1.0f, 1.0f};              // albedo tint (glTF baseColorFactor)
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float ao        = 1.0f;
};

} // namespace iron
