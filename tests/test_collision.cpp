#include "test_framework.h"
#include "game/Collision.h"
#include "math/Aabb.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    const Aabb box{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};

    // Sphere center inside the box: always overlaps.
    {
        CHECK(sphereOverlapAabb(Vec3{0.0f, 0.0f, 0.0f}, 0.1f, box));
    }

    // Sphere center just outside the +X face, within radius: overlaps.
    {
        // Center is at x=1.5 (0.5 outside face), radius 0.6: overlap.
        CHECK(sphereOverlapAabb(Vec3{1.5f, 0.0f, 0.0f}, 0.6f, box));
    }

    // Sphere center outside +X face beyond its radius: miss.
    {
        // Center at x=2.0 (1.0 outside face), radius 0.5: miss.
        CHECK(!sphereOverlapAabb(Vec3{2.0f, 0.0f, 0.0f}, 0.5f, box));
    }

    // Sphere near a corner — distance is sqrt(3 * 0.5^2) ≈ 0.866. Radius 1.0
    // overlaps the corner; radius 0.5 does not.
    {
        const Vec3 corner{1.5f, 1.5f, 1.5f};
        CHECK(sphereOverlapAabb(corner, 1.0f, box));
        CHECK(!sphereOverlapAabb(corner, 0.5f, box));
    }

    return iron_test_result();
}
