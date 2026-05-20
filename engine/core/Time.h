#pragma once

namespace iron {

// Per-frame timing values handed to update/render callbacks.
struct FrameTime {
    float deltaSeconds = 0.0f;   // simulation step length (fixed)
    float totalSeconds = 0.0f;   // seconds since the loop started
};

} // namespace iron
