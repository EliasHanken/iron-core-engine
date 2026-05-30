#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"

namespace iron {

struct Transform {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale    = {1.0f, 1.0f, 1.0f};
};

}  // namespace iron
