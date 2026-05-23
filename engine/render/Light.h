#pragma once

#include "math/Vec.h"

namespace iron {

// A single directional light — like the sun: parallel rays, no position, the
// same everywhere. `direction` is the direction the light travels.
struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f};  // pointing straight down by default
    Vec3 color{1.0f, 1.0f, 1.0f};       // light colour / intensity
    float ambient = 0.1f;               // flat fill term, 0..1
};

// A single point light — like a lantern or a torch. Omnidirectional,
// falls off with distance, no shadow casting in this milestone.
//
// Falloff is range-based smoothstep: contribution goes from full at the
// light's position to zero at `range`. Picking `range` is how you author
// a point light's reach — one intuitive parameter, predictable cutoff.
struct PointLight {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
};

} // namespace iron
