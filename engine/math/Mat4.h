#pragma once

#include "math/Vec.h"

namespace iron {

// A 4x4 matrix stored column-major (OpenGL's convention), so the 16 floats can
// be uploaded to a shader uniform directly with no transpose.
//
// Memory layout: m[c * 4 + r] is the element at row r, column c. Use at(r, c)
// instead of indexing m directly to keep call sites readable.
struct Mat4 {
    float m[16] = {};

    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 identity() {
        Mat4 r;
        r.at(0, 0) = 1.0f;
        r.at(1, 1) = 1.0f;
        r.at(2, 2) = 1.0f;
        r.at(3, 3) = 1.0f;
        return r;
    }
};

// Matrix * matrix. result = a * b means "apply b, then a" to a vector.
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            r.at(row, col) = sum;
        }
    }
    return r;
}

// Matrix * column vector.
inline Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {
        a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w,
        a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w,
        a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w,
        a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w,
    };
}

} // namespace iron
