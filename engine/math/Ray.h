#pragma once

#include "math/Vec.h"
#include "math/Aabb.h"

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

// Ray vs axis-aligned box (the slab method). If the ray hits the box at or in
// front of its origin, sets `outT` to the entry distance and returns true. A
// ray whose origin is inside the box reports t = 0.
inline bool intersectRayAabb(const Ray& ray, const Aabb& box, float& outT) {
    float tMin = 0.0f;
    float tMax = 1e30f;

    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float boxMin[3] = {box.min.x, box.min.y, box.min.z};
    const float boxMax[3] = {box.max.x, box.max.y, box.max.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < 1e-8f) {
            // Ray parallel to this slab: miss if the origin is outside it.
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) {
                return false;
            }
        } else {
            const float inv = 1.0f / dir[axis];
            float t1 = (boxMin[axis] - origin[axis]) * inv;
            float t2 = (boxMax[axis] - origin[axis]) * inv;
            if (t1 > t2) {
                const float tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if (t1 > tMin) tMin = t1;
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) {
                return false;  // the slabs do not overlap: miss
            }
        }
    }
    outT = tMin;
    return true;
}

} // namespace iron
