#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// A world-space mirror plane: every point on one side reflects to the
// other. `normal` must be unit-length; `d` is the signed distance to the
// origin along the normal. The plane is { p : dot(p, normal) = d }.
//
// Example: normal = {0,1,0}, d = -3 is the horizontal plane at y = -3.
struct ReflectionPlane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float d = 0.0f;
};

// Returns a 4x4 matrix that reflects any point across the plane. The
// matrix is its own inverse. Reflecting a point on the plane returns
// the same point.
Mat4 reflectionMatrix(const ReflectionPlane& plane);

} // namespace iron
