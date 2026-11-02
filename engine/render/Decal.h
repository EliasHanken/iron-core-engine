#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>

namespace iron {

// A decal: an image projected onto whatever surfaces fall inside a box, changing
// their colour, roughness and glow before they're lit, so it sits in the surface
// rather than floating on it. Bullet holes, scorch marks, grime, painted signs.
//
// `transform` places a unit cube (-0.5..0.5 on each axis) in the world: x and y
// span the image, and the image projects along -z, onto surfaces facing +z.
// Surfaces facing away are left alone, and those at a glancing angle fade out.
struct Decal {
    Mat4          transform = Mat4::identity();
    std::uint32_t layer = 0;         // Which of the decal textures (setDecalTextures).
    Vec3          tint{1.0f, 1.0f, 1.0f};
    float         opacity = 1.0f;
    float         roughness = -1.0f;  // The surface's roughness where it shows, or < 0 to keep it.
    Vec3          glow{0.0f, 0.0f, 0.0f};  // Emitted light where it shows: hot metal, embers.
};

}  // namespace iron
