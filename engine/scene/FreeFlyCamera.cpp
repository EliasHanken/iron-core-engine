#include "scene/FreeFlyCamera.h"

#include "math/Transform.h"

#include <cmath>

namespace iron {

namespace {
constexpr float kPitchLimit = 1.55334f;  // ~89 degrees
}

Vec3 FreeFlyCamera::forward() const {
    // yaw=0, pitch=0 -> (0, 0, -1). yaw rotates around +Y, pitch tilts up/down.
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    return Vec3{-sy * cp, sp, -cy * cp};
}

Mat4 FreeFlyCamera::viewMatrix() const {
    const Vec3 f = forward();
    const Vec3 target = Vec3{position.x + f.x, position.y + f.y, position.z + f.z};
    return lookAt(position, target, Vec3{0.0f, 1.0f, 0.0f});
}

void FreeFlyCamera::update(float dt,
                           float mouseDx, float mouseDy,
                           bool fwd, bool back, bool left, bool right,
                           bool worldDown, bool worldUp,
                           float moveSpeed,
                           float mouseSensitivity) {
    // Mouse: mouseDx>0 (cursor moved right) -> turn right -> yaw decreases.
    // mouseDy>0 (cursor moved down)  -> look down -> pitch decreases.
    yaw   -= mouseDx * mouseSensitivity;
    pitch -= mouseDy * mouseSensitivity;
    if (pitch >  kPitchLimit) pitch =  kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;

    const Vec3 f = forward();
    // Horizontal right vector: f x worldUp projected to the ground plane,
    // normalised. (Strafe is always horizontal regardless of pitch.)
    // worldUp = (0, 1, 0); f x worldUp = (-f.z, 0, f.x)
    Vec3 r{-f.z, 0.0f, f.x};
    const float rLen = std::sqrt(r.x * r.x + r.z * r.z);
    if (rLen > 1e-6f) {
        r.x /= rLen;
        r.z /= rLen;
    }

    const float step = moveSpeed * dt;
    if (fwd)       { position.x += f.x * step; position.y += f.y * step; position.z += f.z * step; }
    if (back)      { position.x -= f.x * step; position.y -= f.y * step; position.z -= f.z * step; }
    if (right)     { position.x += r.x * step; position.z += r.z * step; }
    if (left)      { position.x -= r.x * step; position.z -= r.z * step; }
    if (worldUp)   { position.y += step; }
    if (worldDown) { position.y -= step; }
}

} // namespace iron
