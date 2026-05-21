#include "test_framework.h"
#include "math/Vec.h"
#include "physics/VerletPoint.h"

using namespace iron;

int main() {
    // A point at rest with no acceleration stays put.
    {
        VerletPoint p;
        p.position = Vec3{1.0f, 2.0f, 3.0f};
        p.previousPosition = Vec3{1.0f, 2.0f, 3.0f};
        integrate(p, Vec3{0.0f, 0.0f, 0.0f}, 0.1f);
        CHECK_NEAR(p.position.x, 1.0f);
        CHECK_NEAR(p.position.y, 2.0f);
        CHECK_NEAR(p.position.z, 3.0f);
    }

    // One step under gravity moves the point by acceleration * dt * dt.
    {
        VerletPoint p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.previousPosition = Vec3{0.0f, 0.0f, 0.0f};
        integrate(p, Vec3{0.0f, -10.0f, 0.0f}, 0.5f);
        CHECK_NEAR(p.position.y, -2.5f);  // -10 * 0.5 * 0.5
    }

    // Implicit velocity carries the point forward (inertia), and
    // previousPosition rolls forward to the old position.
    {
        VerletPoint p;
        p.position = Vec3{1.0f, 0.0f, 0.0f};
        p.previousPosition = Vec3{0.0f, 0.0f, 0.0f};  // moving +X by 1 per step
        integrate(p, Vec3{0.0f, 0.0f, 0.0f}, 0.1f);
        CHECK_NEAR(p.position.x, 2.0f);
        CHECK_NEAR(p.previousPosition.x, 1.0f);
    }

    // A pinned point never moves under integration.
    {
        VerletPoint p;
        p.position = Vec3{5.0f, 5.0f, 5.0f};
        p.previousPosition = Vec3{5.0f, 5.0f, 5.0f};
        p.pinned = true;
        integrate(p, Vec3{0.0f, -10.0f, 0.0f}, 1.0f);
        CHECK_NEAR(p.position.y, 5.0f);
    }

    return iron_test_result();
}
