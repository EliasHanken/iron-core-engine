#pragma once

#include "math/Vec.h"

namespace iron {

// What a film camera does to an image, applied while compositing. Off by default,
// so that games which don't ask for it keep the plain composite. SSAO isn't applied
// on this path, so turn its strength down when enabling this.
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

    // Depth of field. A blur of 0 turns it off.
    float focusDistance = 10.0f;       // Metres from the camera.
    float focusRange = 4.0f;           // Metres around it that stay sharp.
    float blurRadius = 0.0f;           // Widest blur, as a fraction of screen height.

    // What the eye follows, in screen UV. Particles don't write depth, so depth of
    // field alone would blur a spark by whatever wall is behind it. Inside
    // subjectRadius (a fraction of screen height) the picture stays sharp.
    Vec2  subject{0.5f, 0.5f};
    float subjectRadius = 0.0f;
    float heatHaze = 0.0f;             // Rippling air around the subject, 0..1.
};

}  // namespace iron
