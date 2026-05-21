#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cmath>

namespace iron {

// Builders for the standard transform and projection matrices. All return
// column-major Mat4 ready to upload to a shader.

inline Mat4 translation(Vec3 t) {
    Mat4 m = Mat4::identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

inline Mat4 scaling(Vec3 s) {
    Mat4 m = Mat4::identity();
    m.at(0, 0) = s.x;
    m.at(1, 1) = s.y;
    m.at(2, 2) = s.z;
    return m;
}

inline Mat4 rotationX(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(1, 1) = c;  m.at(1, 2) = -s;
    m.at(2, 1) = s;  m.at(2, 2) = c;
    return m;
}

inline Mat4 rotationY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = c;   m.at(0, 2) = s;
    m.at(2, 0) = -s;  m.at(2, 2) = c;
    return m;
}

inline Mat4 rotationZ(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = c;  m.at(0, 1) = -s;
    m.at(1, 0) = s;  m.at(1, 1) = c;
    return m;
}

// View matrix: transforms world space into camera space. The camera sits at
// `eye`, looks toward `center`, with `up` giving roll. Right-handed: the
// camera looks down its local -Z.
inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 forward = normalize(center - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    Mat4 m = Mat4::identity();
    m.at(0, 0) = right.x;    m.at(0, 1) = right.y;    m.at(0, 2) = right.z;
    m.at(1, 0) = trueUp.x;   m.at(1, 1) = trueUp.y;   m.at(1, 2) = trueUp.z;
    m.at(2, 0) = -forward.x; m.at(2, 1) = -forward.y; m.at(2, 2) = -forward.z;
    m.at(0, 3) = -dot(right, eye);
    m.at(1, 3) = -dot(trueUp, eye);
    m.at(2, 3) = dot(forward, eye);
    return m;
}

// Perspective projection. `fovYRadians` is the vertical field of view, `aspect`
// is width/height. Maps the view frustum into OpenGL clip space with depth in
// [-1, 1]. Matches the right-handed, -Z-forward convention of lookAt.
inline Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 m;  // all zeros
    m.at(0, 0) = f / aspect;
    m.at(1, 1) = f;
    m.at(2, 2) = (farZ + nearZ) / (nearZ - farZ);
    m.at(2, 3) = (2.0f * farZ * nearZ) / (nearZ - farZ);
    m.at(3, 2) = -1.0f;
    return m;
}

} // namespace iron
