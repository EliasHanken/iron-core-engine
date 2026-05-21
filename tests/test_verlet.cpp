#include "test_framework.h"
#include "math/Vec.h"
#include "physics/VerletPoint.h"
#include "physics/DistanceConstraint.h"
#include "physics/Rope.h"

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

    // A stretched constraint pulls two free points back to rest length,
    // each moving equally toward the centre.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[1].position = Vec3{10.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 4.0f};
        satisfy(c, pts);
        CHECK_NEAR(length(pts[1].position - pts[0].position), 4.0f);
        CHECK_NEAR(pts[0].position.x, 3.0f);
        CHECK_NEAR(pts[1].position.x, 7.0f);
    }

    // A compressed constraint pushes two free points apart to rest length.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[1].position = Vec3{1.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 5.0f};
        satisfy(c, pts);
        CHECK_NEAR(length(pts[1].position - pts[0].position), 5.0f);
    }

    // With one endpoint pinned, only the free point moves — by the full
    // correction — and the pinned point stays exactly put.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[0].pinned = true;
        pts[1].position = Vec3{10.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 4.0f};
        satisfy(c, pts);
        CHECK_NEAR(pts[0].position.x, 0.0f);
        CHECK_NEAR(pts[1].position.x, 4.0f);
    }

    // Iterating a chain of constraints converges every segment to rest length.
    {
        std::vector<VerletPoint> pts(3);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[0].pinned = true;
        pts[1].position = Vec3{5.0f, 0.0f, 0.0f};
        pts[2].position = Vec3{20.0f, 0.0f, 0.0f};
        DistanceConstraint c01{0, 1, 2.0f};
        DistanceConstraint c12{1, 2, 2.0f};
        for (int i = 0; i < 50; ++i) {
            satisfy(c01, pts);
            satisfy(c12, pts);
        }
        CHECK_NEAR(length(pts[1].position - pts[0].position), 2.0f);
        CHECK_NEAR(length(pts[2].position - pts[1].position), 2.0f);
    }

    // A rope has segments + 1 points, and both ends are pinned.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 4, 10.0f);
        CHECK(static_cast<int>(rope.points().size()) == 5);
        CHECK(rope.points().front().pinned);
        CHECK(rope.points().back().pinned);
    }

    // The pinned endpoints stay where setEndpoint puts them, even after update.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 8, 12.0f);
        rope.setEndpointA(Vec3{1.0f, 5.0f, 0.0f});
        rope.setEndpointB(Vec3{9.0f, 5.0f, 0.0f});
        rope.update(1.0f / 60.0f);
        CHECK_NEAR(rope.points().front().position.x, 1.0f);
        CHECK_NEAR(rope.points().front().position.y, 5.0f);
        CHECK_NEAR(rope.points().back().position.x, 9.0f);
        CHECK_NEAR(rope.points().back().position.y, 5.0f);
    }

    // A slack rope's interior sags downward under gravity over time.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 8, 20.0f);
        const float startY = rope.points()[4].position.y;  // middle point
        for (int i = 0; i < 30; ++i) {
            rope.update(1.0f / 60.0f);
        }
        CHECK(rope.points()[4].position.y < startY);
    }

    return iron_test_result();
}
