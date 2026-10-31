#pragma once

#include "math/Vec.h"
#include "scene/Mesh.h"

#include <array>
#include <cstdint>
#include <vector>

namespace genie {

// Everything solid in the shot is generated here, so the demo ships no art.

inline constexpr float kLampScale = 1.5f;

// The dune surface. groundHeight() in GenieShaders.h is the GLSL copy, and the
// two must stay in lockstep or smoke will stop fading out at the sand.
float groundHeight(float x, float z);

// World-space position of the lamp's origin: resting on the sand.
iron::Vec3 lampPosition();

// World-space tip of the lamp's spout, where the plasma leaves.
iron::Vec3 spoutTip();

iron::MeshData makeDunes();
iron::MeshData makeLamp();  // Lamp-local units; draw with kLampScale at lampPosition().

// Tileable wind ripples for the sand, RGBA8 tangent-space normals. `size` must
// be a positive power of two.
std::vector<std::uint8_t> makeSandNormalMap(int size);

// Night sky cubemap faces, RGBA8, ordered +X -X +Y -Y +Z -Z. Nearly black, with
// a dim violet horizon that the lamp's metal can reflect.
std::array<std::vector<std::uint8_t>, 6> makeNightSkyFaces(int size);

}  // namespace genie
