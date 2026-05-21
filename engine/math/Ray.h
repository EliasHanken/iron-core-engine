#pragma once

#include "math/Vec.h"

#include <cmath>

namespace iron {

// A ray for picking and line-of-sight queries. `direction` is expected to be
// unit length.
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Ray vs sphere. If the ray hits the sphere at or in front of its origin,
// sets `outT` to the distance along the ray of the nearest intersection and
// returns true. A ray whose origin is inside the sphere reports t = 0.
inline bool intersectRaySphere(const Ray& ray, Vec3 center, float radius,
                               float& outT) {
    const Vec3 m = ray.origin - center;
    const float b = dot(m, ray.direction);
    const float c = dot(m, m) - radius * radius;

    // Origin outside the sphere (c > 0) and the ray pointing away (b > 0): miss.
    if (c > 0.0f && b > 0.0f) {
        return false;
    }
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return false;  // the ray misses the sphere entirely
    }

    float t = -b - std::sqrt(discriminant);
    if (t < 0.0f) {
        t = 0.0f;  // the ray started inside the sphere
    }
    outT = t;
    return true;
}

} // namespace iron
