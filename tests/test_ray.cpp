#include "test_framework.h"
#include "math/Ray.h"
#include "math/Aabb.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Direct hit: ray along +Z, sphere ahead at z = 10, radius 1.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        const bool hit = intersectRaySphere(ray, Vec3{0.0f, 0.0f, 10.0f}, 1.0f, t);
        CHECK(hit);
        CHECK_NEAR(t, 9.0f);  // nearest surface is at z = 9
    }

    // Miss: ray points away from the sphere.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        float t = -1.0f;
        CHECK(!intersectRaySphere(ray, Vec3{0.0f, 0.0f, 10.0f}, 1.0f, t));
    }

    // Miss: ray passes to the side of the sphere.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        CHECK(!intersectRaySphere(ray, Vec3{5.0f, 0.0f, 10.0f}, 1.0f, t));
    }

    // Origin inside the sphere: hit reported at t = 0.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        const bool hit = intersectRaySphere(ray, Vec3{0.0f, 0.0f, 0.0f}, 5.0f, t);
        CHECK(hit);
        CHECK_NEAR(t, 0.0f);
    }

    // Glancing hit near the rim still registers.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        CHECK(intersectRaySphere(ray, Vec3{0.99f, 0.0f, 10.0f}, 1.0f, t));
    }

    // intersectRayAabb: direct hit on a box ahead along +Z.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        const bool hit = intersectRayAabb(ray, box, t);
        CHECK(hit);
        CHECK_NEAR(t, 5.0f);  // entry face is at z = 5
    }

    // Miss: ray points away from the box.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }

    // Miss: ray passes well beside the box.
    {
        Ray ray{Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }

    // Origin inside the box: hit reported at t = 0.
    {
        Ray ray{Vec3{0.0f, 0.0f, 6.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        const bool hit = intersectRayAabb(ray, box, t);
        CHECK(hit);
        CHECK_NEAR(t, 0.0f);
    }

    // Ray parallel to a slab but outside it: miss.
    {
        Ray ray{Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};  // y=5, box y in [-1,1]
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }

    // intersectRayAabb normal overload: +Z face entry.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        Vec3 n{};
        const bool hit = intersectRayAabb(ray, box, t, n);
        CHECK(hit);
        CHECK_NEAR(t, 5.0f);
        CHECK_NEAR(n.x, 0.0f);
        CHECK_NEAR(n.y, 0.0f);
        CHECK_NEAR(n.z, -1.0f);  // entry face faces back at -Z
    }

    // intersectRayAabb normal overload: +X face entry from below the box.
    {
        Ray ray{Vec3{-5.0f, 0.0f, 6.0f}, Vec3{1.0f, 0.0f, 0.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        Vec3 n{};
        const bool hit = intersectRayAabb(ray, box, t, n);
        CHECK(hit);
        CHECK_NEAR(t, 4.0f);
        CHECK_NEAR(n.x, -1.0f);
        CHECK_NEAR(n.y, 0.0f);
        CHECK_NEAR(n.z, 0.0f);
    }

    return iron_test_result();
}
