#pragma once

#include <cmath>

namespace iron {

// Plain-old-data vectors. Hand-written to learn the math; no SIMD, no
// cleverness. All operations are free functions or member operators so the
// types stay trivial and copyable.

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// --- Vec2 ---
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }

// --- Vec3 ---
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

// Dot product: measures how aligned two vectors are.
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Cross product: a vector perpendicular to both inputs (right-handed).
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }

// Returns a unit-length vector. A zero vector is returned unchanged.
inline Vec3 normalize(Vec3 v) {
    const float len = length(v);
    if (len <= 1e-8f) {
        return v;
    }
    return v * (1.0f / len);
}

// Linear interpolation between two Vec3 by parameter t.
// t=0 returns a, t=1 returns b. No clamping — callers may pass values
// outside [0,1] for extrapolation if they really mean it.
inline Vec3 interpolate(Vec3 a, Vec3 b, float t) {
    return Vec3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

// --- Vec4 ---
inline Vec4 operator+(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 operator-(Vec4 a, Vec4 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline Vec4 operator*(Vec4 v, float s) {
    return {v.x * s, v.y * s, v.z * s, v.w * s};
}

} // namespace iron
