#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "scene/Picking.h"
#include "test_framework.h"

#include <vector>

using namespace iron;

int main() {
    // Mat4 inverse round-trip: M * inverse(M) ~= identity.
    {
        const Mat4 M = perspective(1.0472f, 1.3333f, 0.1f, 100.0f)
                     * translation(Vec3{1.0f, -2.0f, 3.0f});
        const Mat4 prod = M * inverse(M);
        const Mat4 I = Mat4::identity();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK_NEAR(prod.at(r, c), I.at(r, c));
    }

    // screenPointToRay: identity view + perspective; center pixel -> forward -Z.
    {
        const Mat4 view = Mat4::identity();
        const Mat4 proj = perspective(1.0472f, 800.0f / 600.0f, 0.1f, 100.0f);
        const Ray r = screenPointToRay(view, proj, Vec2{400.0f, 300.0f},
                                       Vec2{800.0f, 600.0f}, Vec3{0.0f, 0.0f, 0.0f});
        CHECK_NEAR(r.direction.x, 0.0f);
        CHECK_NEAR(r.direction.y, 0.0f);
        CHECK_NEAR(r.direction.z, -1.0f);
        // Right-edge pixel tilts the ray toward +X.
        const Ray rr = screenPointToRay(view, proj, Vec2{800.0f, 300.0f},
                                        Vec2{800.0f, 600.0f}, Vec3{0.0f, 0.0f, 0.0f});
        CHECK(rr.direction.x > 0.0f);
    }

    // pickEntity: nearest hit AABB along the ray; miss -> -1.
    {
        std::vector<Aabb> boxes = {
            Aabb{Vec3{-0.5f, -0.5f, -11.0f}, Vec3{0.5f, 0.5f, -9.0f}},  // far
            Aabb{Vec3{-0.5f, -0.5f, -1.0f},  Vec3{0.5f, 0.5f, 1.0f}},   // near
        };
        const Ray down{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        CHECK(pickEntity(down, boxes) == 1);  // the near box
        const Ray up{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 1.0f, 0.0f}};
        CHECK(pickEntity(up, boxes) == -1);   // misses both
    }

    // pickEntity skips the box the ray origin is inside: a camera inside the near
    // box, looking at the far box, selects the far box (not the one it's in).
    {
        std::vector<Aabb> boxes = {
            Aabb{Vec3{-0.5f, -0.5f, -11.0f}, Vec3{0.5f, 0.5f, -9.0f}},  // far
            Aabb{Vec3{-1.0f, -1.0f, -1.0f},  Vec3{1.0f, 1.0f, 1.0f}},   // contains the origin
        };
        const Ray inside{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        CHECK(pickEntity(inside, boxes) == 0);  // the far box, not the enclosing one
    }
    return iron_test_result();
}
