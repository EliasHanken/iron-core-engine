#include "math/Quaternion.h"

#include <cmath>

namespace iron {

namespace {

// Build a quaternion from a 3x3 orthonormal rotation expressed as three
// column vectors (xAxis, yAxis, zAxis). Shepperd's method — pick the
// branch whose trace term is the largest positive value so the square
// root never operates on a near-zero number.
Quat quatFromBasis(Vec3 xAxis, Vec3 yAxis, Vec3 zAxis) {
    const float m00 = xAxis.x, m01 = yAxis.x, m02 = zAxis.x;
    const float m10 = xAxis.y, m11 = yAxis.y, m12 = zAxis.y;
    const float m20 = xAxis.z, m21 = yAxis.z, m22 = zAxis.z;

    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4 * q.w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;  // s = 4 * q.x
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;  // s = 4 * q.y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;  // s = 4 * q.z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q.normalized();
}

}  // namespace

Quat quatLookAt(Vec3 eye, Vec3 target, Vec3 up) {
    // Right-handed camera basis. The camera looks down its local -Z, so we
    // store the forward direction as `zAxis = -lookDir` (the local +Z axis
    // points behind the camera). Right = up × zAxis. Real up = zAxis × right.
    Vec3 lookDir = target - eye;
    const float lookLen = length(lookDir);
    if (lookLen <= 1e-6f) {
        // Degenerate: eye == target. Return identity rather than NaN.
        return Quat::identity();
    }
    lookDir = lookDir * (1.0f / lookLen);

    Vec3 upN = normalize(up);
    // If `up` is colinear with the look direction, fall back to +Z.
    if (std::fabs(dot(upN, lookDir)) > 0.9995f) {
        upN = Vec3{0.0f, 0.0f, 1.0f};
        if (std::fabs(dot(upN, lookDir)) > 0.9995f) {
            // Still colinear (caller asked for +Z look and +Z up). Use +X.
            upN = Vec3{1.0f, 0.0f, 0.0f};
        }
    }

    const Vec3 zAxis = lookDir * -1.0f;          // local +Z (behind camera)
    const Vec3 xAxis = normalize(cross(upN, zAxis));
    const Vec3 yAxis = cross(zAxis, xAxis);      // already unit; right × forward
    return quatFromBasis(xAxis, yAxis, zAxis);
}

}  // namespace iron
