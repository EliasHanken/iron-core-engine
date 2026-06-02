#pragma once

#include "render/Handles.h"

#include <string>
#include <vector>

namespace iron {

class Renderer;

// Inverts the R, G, B channels of an RGBA byte buffer in place
// (channel = 255 - channel). Alpha is left untouched.
//
// `pixels.size()` should be a multiple of 4; trailing bytes (if any) are
// ignored. An empty buffer is a no-op.
void invertRGBChannels(std::vector<unsigned char>& pixels);

// Builds the combined metallic-roughness texture (glTF layout):
//   R = 255, G = roughness (from the grayscale roughness PNG), B = metallicConstant,
//   A = 255. Loaded LINEAR (srgb=false). For CC0 packs that ship only a
//   roughness map and no metallic map: pass metallicConstant=0.0f for
//   dielectric materials (wood, brick, ground) or 1.0f for metals.
// Returns kInvalidHandle if the roughness PNG fails to load.
TextureHandle loadMetallicRoughness(Renderer& r, const std::string& roughnessPath,
                                    float metallicConstant);

} // namespace iron
