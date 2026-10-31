#pragma once

#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iron {
class FontAtlas;
}

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
    float shadowRadius = 1.0f;  // Sphere that stands in for it when casting shadows.
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

// How much of a spherical light can be seen from `eye` past `spheres` (xyz =
// centre, w = radius), from 0 to 1. Drives the lens flare, so that it flickers as
// rock crosses the star.
float lightVisibility(iron::Vec3 eye, iron::Vec3 light, float lightRadius,
                      std::span<const iron::Vec4> spheres);

// Starting state for a title made of particles, 16 floats per particle: `a.xy`
// holds where it lands in clip space, a.z when it starts (0 at the left), a.w a
// random value. The text is centred, `width` is clip-space width, and `aspect`
// keeps letters from stretching.
std::vector<float> makeTitleParticles(const iron::FontAtlas& font, const std::string& text,
                                      int count, float width, float baselineY, float tracking,
                                      float aspect, std::uint32_t seed);

// Tileable rock detail, RGBA8 tangent-space normals. `size` is a power of two.
std::vector<std::uint8_t> makeRockNormalMap(int size);

}  // namespace ironcore
