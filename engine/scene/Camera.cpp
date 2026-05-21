#include "scene/Camera.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kPitchLimit = kPi * 0.49f;  // just shy of straight up/down
} // namespace

void Camera::orbit(float deltaYaw, float deltaPitch) {
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;
}

void Camera::zoom(float factor) {
    distance_ *= factor;
    if (distance_ < kMinDistance) distance_ = kMinDistance;
    if (distance_ > kMaxDistance) distance_ = kMaxDistance;
}

Vec3 Camera::position() const {
    // Spherical to Cartesian, offset from the target.
    const float cp = std::cos(pitch_);
    const Vec3 offset{
        distance_ * cp * std::sin(yaw_),
        distance_ * std::sin(pitch_),
        distance_ * cp * std::cos(yaw_),
    };
    return target_ + offset;
}

Mat4 Camera::viewMatrix() const {
    return lookAt(position(), target_, Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projectionMatrix() const {
    return perspective(fovY_, aspect_, nearZ_, farZ_);
}

} // namespace iron
