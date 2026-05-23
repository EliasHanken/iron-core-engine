#pragma once

#include "math/Vec.h"

namespace iron {

// Distance fog: every lit fragment blends toward `color` with weight
// `1 - exp(-density * distFromCamera)`. The skybox pass also blends
// toward this colour near the horizon to dissolve the sky/terrain edge.
//
// `density = 0` (the default) disables fog entirely — existing demos
// pass a default-constructed Fog and see no visual change.
struct Fog {
    Vec3 color{0.7f, 0.6f, 0.5f}; // warm-grey by default
    float density = 0.0f;          // 0 = no fog
};

} // namespace iron
