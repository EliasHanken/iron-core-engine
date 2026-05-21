#pragma once

#include "math/Vec.h"

namespace iron {

// An axis-aligned bounding box, defined by its minimum and maximum corners.
struct Aabb {
    Vec3 min;
    Vec3 max;
};

} // namespace iron
