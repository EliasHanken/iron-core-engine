#include "math/Transform.h"
#include "render/Culling.h"
#include "scene/Mesh.h"
#include "scene/MeshLod.h"

#include "test_framework.h"

#include <vector>

using namespace iron;

static Mat4 camera() {
    // At the origin, looking down -z, 90 degrees tall, square.
    return perspective(1.5707963f, 1.0f, 0.1f, 100.0f) * lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
}

static void spheres_in_front_are_visible() {
    const Frustum f = frustumFromViewProjection(camera());
    CHECK(sphereInFrustum(f, {0, 0, -10}, 1.0f));
    CHECK(sphereInFrustum(f, {9.0f, 0, -10}, 1.0f));  // Near the right edge (45 degrees).
}

static void spheres_outside_are_culled() {
    const Frustum f = frustumFromViewProjection(camera());
    CHECK(!sphereInFrustum(f, {0, 0, 10}, 1.0f));      // Behind.
    CHECK(!sphereInFrustum(f, {20.0f, 0, -10}, 1.0f));  // Far right.
    CHECK(!sphereInFrustum(f, {0, 30.0f, -10}, 1.0f));  // Far above.
    CHECK(!sphereInFrustum(f, {0, 0, -150}, 1.0f));     // Past the far plane.
}

static void a_sphere_straddling_a_plane_stays() {
    const Frustum f = frustumFromViewProjection(camera());
    // Its centre is just outside the right plane, but it reaches back in.
    CHECK(sphereInFrustum(f, {11.0f, 0, -10}, 1.5f));
    CHECK(!sphereInFrustum(f, {11.0f, 0, -10}, 0.5f));
}

static void planes_face_inward_and_are_unit() {
    const Frustum f = frustumFromViewProjection(camera());
    for (const Vec4& p : f) {
        CHECK_NEAR(p.x * p.x + p.y * p.y + p.z * p.z, 1.0f);
        CHECK(p.x * 0.0f + p.y * 0.0f + p.z * -10.0f + p.w > 0.0f);  // A point in view is inside every plane.
    }
}

static void size_falls_with_distance() {
    const float near = projectedPixels({0, 0, -5}, 1.0f, {0, 0, 0}, 540.0f);
    const float far = projectedPixels({0, 0, -50}, 1.0f, {0, 0, 0}, 540.0f);
    CHECK_NEAR(near, 216.0f);
    CHECK_NEAR(far, 21.6f);
}

static void lods_step_down_with_size() {
    const std::vector<float> t = {120.0f, 40.0f, 12.0f};
    CHECK(selectLod(500.0f, t, 4) == 0);
    CHECK(selectLod(119.0f, t, 4) == 1);
    CHECK(selectLod(39.0f, t, 4) == 2);
    CHECK(selectLod(5.0f, t, 4) == 3);
    CHECK(selectLod(5.0f, t, 2) == 1);  // Capped by the levels a mesh has.
    CHECK(selectLod(5.0f, t, 1) == 0);
}

static void a_sphere_simplifies_into_a_chain() {
    const MeshData sphere = makeUVSphere(1.0f, 48);
    const auto lods = generateLods(sphere, 4);
    CHECK(lods.size() >= 3);
    for (std::size_t i = 1; i < lods.size(); ++i) {
        CHECK(lods[i].indices.size() < lods[i - 1].indices.size());
        CHECK(lods[i].indices.size() % 3 == 0);
        CHECK(lods[i].vertices.size() == lods[0].vertices.size());  // Shared vertices.
        for (std::uint32_t index : lods[i].indices) CHECK(index < lods[i].vertices.size());
    }
    CHECK(lods[0].indices.size() == sphere.indices.size());
}

static void a_box_stays_one_level() {
    const MeshData box = makeCube();
    const auto lods = generateLods(box, 4);
    CHECK(lods.size() == 1);
}

int main() {
    spheres_in_front_are_visible();
    spheres_outside_are_culled();
    a_sphere_straddling_a_plane_stays();
    planes_face_inward_and_are_unit();
    size_falls_with_distance();
    lods_step_down_with_size();
    a_sphere_simplifies_into_a_chain();
    a_box_stays_one_level();
    return iron_test_result();
}
