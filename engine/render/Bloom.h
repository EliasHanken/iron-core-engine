#pragma once

#include "math/Vec.h"

#include <algorithm>

namespace iron {

// Soft-knee bright-pass prefilter (Call of Duty / Unreal). Returns the input
// color scaled by its bloom contribution: 0 below (threshold - knee), a smooth
// quadratic ramp across the knee, and ~linear passthrough above the threshold.
// Keep in lockstep with the GLSL prefilter() in VkPostProcess.cpp.
inline Vec3 bloomPrefilter(Vec3 c, float threshold, float knee) {
    const float br = std::max(c.x, std::max(c.y, c.z));
    float soft = br - threshold + knee;
    soft = std::clamp(soft, 0.0f, 2.0f * knee);
    soft = (soft * soft) / (4.0f * knee + 1e-4f);
    float contrib = std::max(soft, br - threshold);
    contrib /= std::max(br, 1e-4f);
    return Vec3{c.x * contrib, c.y * contrib, c.z * contrib};
}

// Karis average weight for firefly reduction: brighter samples are weighted less
// so a single very bright pixel (e.g. the HDR sun) can't dominate a downsample.
// Keep in lockstep with the GLSL karis() in VkPostProcess.cpp.
inline float karisWeight(Vec3 c) {
    const float luma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    return 1.0f / (1.0f + luma);
}

}  // namespace iron
