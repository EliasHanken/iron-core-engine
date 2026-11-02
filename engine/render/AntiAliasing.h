#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>

namespace iron {

// How the renderer smooths edges.
enum class AntiAliasing {
    Off = 0,
    // Temporal: the projection moves by a fraction of a pixel each frame and the
    // frames are blended along the motion vectors, so each pixel averages several
    // points of what it covers. Also calms shimmer on thin and shiny things.
    Temporal = 1,
};

// The Halton sequence's `index`th value (from 1) in base `base`: points in [0, 1)
// that fill the interval evenly however many are taken.
inline float halton(std::uint32_t index, std::uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

// This frame's sub-pixel offset, in pixels within [-0.5, 0.5): the (2, 3) Halton
// points, cycling every `count` frames.
inline Vec2 temporalJitter(std::uint32_t frame, std::uint32_t count = 8) {
    const std::uint32_t i = frame % count + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

// Moves a projection's image by `pixels` on a `width` x `height` target: multiply
// on the left of the projection. Depth and w are untouched.
inline Mat4 jitterMatrix(Vec2 pixels, float width, float height) {
    Mat4 m = Mat4::identity();
    m.at(0, 3) = 2.0f * pixels.x / width;
    m.at(1, 3) = 2.0f * pixels.y / height;
    return m;
}

}  // namespace iron
