#pragma once

#include <algorithm>
#include <cmath>

namespace iron {

// The fluid always advances by this step, so that a given sequence of frames
// produces the same smoke on any machine.
inline constexpr float kFluidStepSeconds = 1.0f / 60.0f;

// A stalled frame must not queue up simulation work, or it would stall the next
// one too.
inline constexpr int kMaxFluidStepsPerFrame = 3;

// How many whole steps to run this frame. The remainder stays in `accumulator`.
inline int fluidStepsThisFrame(float dt, float& accumulator) {
    // The nudge stops float rounding from turning exactly one step into zero.
    constexpr float kRoundingNudge = 1e-5f;

    accumulator += dt;
    const int steps = static_cast<int>(std::floor((accumulator + kRoundingNudge) / kFluidStepSeconds));
    if (steps > kMaxFluidStepsPerFrame) {
        accumulator = 0.0f;
        return kMaxFluidStepsPerFrame;
    }
    accumulator = std::max(accumulator - static_cast<float>(steps) * kFluidStepSeconds, 0.0f);
    return steps;
}

}  // namespace iron
