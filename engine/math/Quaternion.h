#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cmath>

namespace iron {

// A unit quaternion represents a 3D rotation without the gimbal-lock and
// interpolation problems of Euler angles. Stored as (x, y, z, w) where
// (x, y, z) is the vector part and w the scalar part.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static constexpr Quat identity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

    // Rotation of `angleRadians` about a (not necessarily unit) axis.
    static Quat fromAxisAngle(Vec3 axis, float angleRadians) {
        const Vec3 a = normalize(axis);
        const float half = angleRadians * 0.5f;
        const float s = std::sin(half);
        return Quat{a.x * s, a.y * s, a.z * s, std::cos(half)};
    }

    float length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

    Quat normalized() const {
        const float len = length();
        if (len <= 1e-8f) {
            return identity();
        }
        const float inv = 1.0f / len;
        return Quat{x * inv, y * inv, z * inv, w * inv};
    }

    // Rotate a vector by this quaternion: v' = q * v * q^-1, expanded.
    // The expansion is only exact for a unit quaternion, so we normalize
    // first — matching toMat4() and guarding against composition drift.
    Vec3 rotate(Vec3 v) const {
        const Quat q = normalized();
        const Vec3 u{q.x, q.y, q.z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * q.w + cross(u, t);
    }

    // Equivalent rotation as a column-major 4x4 matrix.
    Mat4 toMat4() const {
        const Quat q = normalized();
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        Mat4 m = Mat4::identity();
        m.at(0, 0) = 1.0f - 2.0f * (yy + zz);
        m.at(0, 1) = 2.0f * (xy - wz);
        m.at(0, 2) = 2.0f * (xz + wy);
        m.at(1, 0) = 2.0f * (xy + wz);
        m.at(1, 1) = 1.0f - 2.0f * (xx + zz);
        m.at(1, 2) = 2.0f * (yz - wx);
        m.at(2, 0) = 2.0f * (xz - wy);
        m.at(2, 1) = 2.0f * (yz + wx);
        m.at(2, 2) = 1.0f - 2.0f * (xx + yy);
        return m;
    }
};

// Composition: (a * b) applies b first, then a — same convention as Mat4.
constexpr Quat operator*(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

} // namespace iron
