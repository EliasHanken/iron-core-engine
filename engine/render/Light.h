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

} // namespace iron
