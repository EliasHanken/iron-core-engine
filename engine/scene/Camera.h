#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// An orbit camera: it always looks at a target point and sits on a sphere
// around it, positioned by yaw, pitch, and distance. Good for inspecting a
// single object (the spinning cube).
class Camera {
public:
    void setTarget(Vec3 target) { target_ = target; }
    void setDistance(float distance) { distance_ = distance; }
    void setAspect(float aspect) { aspect_ = aspect; }

    // Add to the orbit angles, in radians (e.g. from mouse drag).
    void orbit(float deltaYaw, float deltaPitch);
    // Multiply the orbit distance (e.g. from scroll); clamped to a sane range.
    void zoom(float factor);

    Vec3 position() const;
    Mat4 viewMatrix() const;
    Mat4 projectionMatrix() const;

private:
    Vec3 target_{0.0f, 0.0f, 0.0f};
    float distance_ = 4.0f;
    float yaw_ = 0.0f;     // radians, around world +Y
    float pitch_ = 0.0f;   // radians, clamped away from the poles
    float aspect_ = 16.0f / 9.0f;
    float fovY_ = 3.14159265358979323846f / 4.0f;  // 45 degrees
    float nearZ_ = 0.1f;
    float farZ_ = 100.0f;
};

} // namespace iron
