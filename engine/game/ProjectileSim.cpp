#include "game/ProjectileSim.h"

#include "math/Ray.h"

#include <cmath>

namespace iron {

std::optional<ProjectileHit> tickProjectile(
    Projectile& p, float dt, std::span<const Aabb> worldBoxes) {
    if (!p.alive || dt <= 0.0f) return std::nullopt;

    const Vec3 delta{p.velocity.x * dt, p.velocity.y * dt, p.velocity.z * dt};
    const float stepLen =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (stepLen < 1e-6f) {
        return std::nullopt;
    }

    const Vec3 dirUnit{delta.x / stepLen, delta.y / stepLen, delta.z / stepLen};
    const Ray ray{p.position, dirUnit};

    bool  bestHit = false;
    float bestT   = stepLen;  // only count hits within this tick's step
    Vec3  bestNormal{};

    for (const auto& box : worldBoxes) {
        float t = 0.0f;
        Vec3 n{};
        if (intersectRayAabb(ray, box, t, n) && t >= 0.0f && t <= bestT) {
            bestHit = true;
            bestT = t;
            bestNormal = n;
        }
    }

    if (bestHit) {
        p.position = Vec3{p.position.x + dirUnit.x * bestT,
                          p.position.y + dirUnit.y * bestT,
                          p.position.z + dirUnit.z * bestT};
        p.alive = false;
        return ProjectileHit{p.position, bestNormal};
    }

    p.position = Vec3{p.position.x + delta.x,
                      p.position.y + delta.y,
                      p.position.z + delta.z};
    return std::nullopt;
}

}  // namespace iron
