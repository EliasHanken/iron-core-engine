#include "scene/FirstPersonController.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
// Just under 90 degrees, so the look direction is never parallel to world up
// (which would make the view matrix degenerate).
constexpr float kPitchLimit = 1.55334f;
}  // namespace

Vec3 FirstPersonController::forwardDir() const {
    const float cp = std::cos(pitch_);
    return Vec3{
        -std::sin(yaw_) * cp,
        std::sin(pitch_),
        -std::cos(yaw_) * cp,
    };
}

Vec3 FirstPersonController::horizontalForward() const {
    return Vec3{-std::sin(yaw_), 0.0f, -std::cos(yaw_)};
}

Vec3 FirstPersonController::horizontalRight() const {
    return Vec3{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
}

void FirstPersonController::update(const ControllerInput& input, float dt) {
    // Mouse look. Moving the mouse up the screen (negative dy) looks up.
    yaw_ += input.mouseDX * mouseSensitivity_;
    pitch_ -= input.mouseDY * mouseSensitivity_;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;

    // Horizontal movement along the look direction flattened onto the ground.
    Vec3 move = horizontalForward() * input.forward
              + horizontalRight() * input.strafe;
    const float len = length(move);
    if (len > 1e-6f) {
        move = move * (1.0f / len);  // normalize so diagonals are not faster
        position_ = position_ + move * (moveSpeed_ * dt);
    }

    // Gravity, then clamp to the flat ground (M2's stand-in for collision).
    velocity_.y += gravity_ * dt;
    position_.y += velocity_.y * dt;
    if (position_.y <= groundHeight_) {
        position_.y = groundHeight_;
        velocity_.y = 0.0f;
    }
}

Vec3 FirstPersonController::eyePosition() const {
    return Vec3{position_.x, position_.y + eyeHeight_, position_.z};
}

Mat4 FirstPersonController::viewMatrix() const {
    const Vec3 eye = eyePosition();
    return lookAt(eye, eye + forwardDir(), Vec3{0.0f, 1.0f, 0.0f});
}

}  // namespace iron
