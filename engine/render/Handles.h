#pragma once

#include <cstdint>

namespace iron {

// Opaque handles into the renderer's resource tables. 0 is "invalid".
// Handle values are (vector index + 1) in the OpenGL backend.
using MeshHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderHandle = std::uint32_t;
// Cubemap textures (skyboxes, environment maps). 0 = invalid.
using CubemapHandle = std::uint32_t;

inline constexpr std::uint32_t kInvalidHandle = 0;

} // namespace iron
