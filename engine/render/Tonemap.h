#pragma once

#include "math/Vec.h"

#include <algorithm>

namespace iron {

// ACES filmic tonemap (Krzysztof Narkowicz fit), CPU port of the GLSL used by
// the post-process composite shaders. `exposure` multiplies the linear input
// before the curve. Result is clamped to [0, 1] per channel.
//
// Keep this in lockstep with the GLSL `aces()` in VkPostProcess.cpp.
inline Vec3 acesFilmic(Vec3 color, float exposure) {
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    auto curve = [](float x) {
        float y = (x * (a * x + b)) / (x * (c * x + d) + e);
        return std::clamp(y, 0.0f, 1.0f);
    };
    return Vec3{curve(color.x * exposure),
               curve(color.y * exposure),
               curve(color.z * exposure)};
}

}  // namespace iron
