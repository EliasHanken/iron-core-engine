#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// Per-step movement intent, supplied by the game from its Input object.
// Keeping the controller free of any GLFW dependency makes it unit-testable.
struct ControllerInput {
    float forward = 0.0f;  // -1 (back) .. +1 (forward)
    float strafe = 0.0f;   // -1 (left) .. +1 (right)
    float mouseDX = 0.0f;  // mouse movement this step, in pixels
    float mouseDY = 0.0f;
};

// First-person player: position and look orientation in one place (for a
// first-person camera the player and the camera are the same thing). Walks on
// a flat ground plane at a configurable height and falls under gravity.
//
// Convention: right-handed; yaw is around world +Y; yaw = 0, pitch = 0 looks
// toward -Z. position_ is the player's feet; the eye sits eyeHeight above it.
class FirstPersonController {
public:
    // Advance the player one fixed simulation step.
    void update(const ControllerInput& input, float dt);

    Mat4 viewMatrix() const;
    Vec3 eyePosition() const;

    void setPosition(Vec3 position) { position_ = position; }
    void setGroundHeight(float y) { groundHeight_ = y; }
    void setEyeHeight(float h) { eyeHeight_ = h; }
    void setMoveSpeed(float unitsPerSecond) { moveSpeed_ = unitsPerSecond; }
    void setMouseSensitivity(float radiansPerPixel) {
        mouseSensitivity_ = radiansPerPixel;
    }

    Vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

private:
    Vec3 forwardDir() const;         // full look direction, including pitch
    Vec3 horizontalForward() const;  // look direction flattened onto the ground
    Vec3 horizontalRight() const;

    Vec3 position_{0.0f, 0.0f, 0.0f};
    Vec3 velocity_{0.0f, 0.0f, 0.0f};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float groundHeight_ = 0.0f;
    float eyeHeight_ = 1.7f;
    float moveSpeed_ = 5.0f;
    float mouseSensitivity_ = 0.0025f;
    float gravity_ = -20.0f;
};

} // namespace iron
