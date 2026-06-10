#pragma once

#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Transform.h"
#include "math/Vec.h"

namespace iron {

struct Transform {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale    = {1.0f, 1.0f, 1.0f};

    // Column-major model matrix M = T * R * S. Factors the composition that was
    // previously inlined at render-submit / picking / gizmo (main.cpp).
    Mat4 matrix() const {
        return translation(position) * rotation.toMat4() * scaling(scale);
    }
};

}  // namespace iron
