#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// Free-flying 6-DOF camera. Engine-side, GLFW-agnostic — the game polls input
// and feeds raw deltas into update().
//
// Convention: right-handed; +Y is up. yaw is around world +Y; yaw = 0,
// pitch = 0 looks toward world -Z. Positive mouseDx (mouse moves right)
// turns the view right (yaw decreases). Pitch is clamped to +/- 89 degrees.
struct FreeFlyCamera {
    Vec3 position{0.0f, 2.0f, 5.0f};
    float yaw = 0.0f;      // radians
    float pitch = 0.0f;    // radians
    float fovDeg = 60.0f;  // for the game's perspective matrix; the camera
                           // itself doesn't build a projection

    // Apply one frame of input.
    //   dt              : seconds since last update
    //   mouseDx,mouseDy : raw pixel deltas this frame
    //   fwd/back/...    : key-pressed-this-frame booleans
    //   moveSpeed       : world units per second when a movement key is held
    //   mouseSensitivity: radians per pixel
    //
    // Movement: fwd/back along camera forward(), left/right along camera
    // right (perpendicular to forward, no Y component), worldUp/Down strictly
    // along world +Y/-Y.
    void update(float dt,
                float mouseDx, float mouseDy,
                bool fwd, bool back, bool left, bool right,
                bool worldDown, bool worldUp,
                float moveSpeed = 5.0f,
                float mouseSensitivity = 0.0025f);

    Mat4 viewMatrix() const;
    Vec3 forward() const;
};

} // namespace iron
