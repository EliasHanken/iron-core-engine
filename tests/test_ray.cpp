#include "test_framework.h"
#include "math/Ray.h"
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

    return iron_test_result();
}
