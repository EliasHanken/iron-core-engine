#pragma once

#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cstdint>
#include <vector>

namespace ironcore {

// All the solid things in the shot, generated at startup.

inline constexpr int kRockCount = 120;
inline constexpr int kRockVariants = 6;
inline constexpr int kShellPlates = 48;

// One piece of rock: where it floats before the eruption, and where it ends up.
struct RockInstance {
    iron::Vec3 home;
    iron::Vec3 axis;       // Tumble axis, unit length.
    float spin = 0.0f;     // Radians per second.
    float scale = 1.0f;
    int variant = 0;
    float ringRadius = 0.0f;
    float ringAngle = 0.0f;
    bool plate = false;    // Part of the core's shell: still until the eruption.
};

std::vector<RockInstance> makeRockField(std::uint32_t seed, int count);

// Pieces of the shell, each centred on its own origin. `plates` receives the
// matching instances, with `home` set to the plate's place on the sphere.
std::vector<iron::MeshData> makeShellPlates(int plateCount, float radius, std::uint32_t seed,
                                            std::vector<RockInstance>& plates);

iron::MeshData makeRock(std::uint32_t seed);

// Pure functions of time, so that any frame can be reproduced exactly.
iron::Vec3 rockPosition(const RockInstance& rock, float t);
float rockAngle(const RockInstance& rock, float t);

// Tileable rock detail, RGBA8 tangent-space normals. `size` is a power of two.
std::vector<std::uint8_t> makeRockNormalMap(int size);

}  // namespace ironcore
