#pragma once

#include "math/Vec.h"

namespace iron {

// What a film camera does to an image, applied while compositing. Off by default,
// so that games which don't ask for it keep the plain composite.
struct CinematicSettings {
    bool  enabled = false;
    float motionBlur = 0.0f;           // 1 = a full 180-degree shutter.
    float chromaticAberration = 0.0f;  // Edge colour split, as a fraction of the screen.
    float vignette = 0.0f;             // Darkening at the corners, 0..1.
    float grain = 0.0f;                // Film grain, 0..1.
    float streak = 0.0f;               // Horizontal anamorphic streaks from bright light.
    float saturation = 1.0f;
    float contrast = 1.0f;
    Vec3  shadowTint{1.0f, 1.0f, 1.0f};     // Multiplies the darks.
    Vec3  highlightTint{1.0f, 1.0f, 1.0f};  // Multiplies the lights.
};

}  // namespace iron
