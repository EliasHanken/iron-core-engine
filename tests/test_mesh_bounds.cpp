#include "math/Aabb.h"
#include "scene/Mesh.h"
#include "test_framework.h"

using namespace iron;

int main() {
    // makeCube() is a unit cube centered at origin -> bounds +/-0.5.
    {
        const Aabb b = meshBounds(makeCube());
        CHECK_NEAR(b.min.x, -0.5f);
        CHECK_NEAR(b.min.y, -0.5f);
        CHECK_NEAR(b.min.z, -0.5f);
        CHECK_NEAR(b.max.x, 0.5f);
        CHECK_NEAR(b.max.y, 0.5f);
        CHECK_NEAR(b.max.z, 0.5f);
    }
    // A hand-built mesh: bounds span the min/max of all vertex positions.
    {
        MeshData m;
        m.vertices.push_back(Vertex{Vec3{-2.0f, 1.0f, 0.0f}, {}, {}, {}});
        m.vertices.push_back(Vertex{Vec3{ 3.0f, -4.0f, 5.0f}, {}, {}, {}});
        m.vertices.push_back(Vertex{Vec3{ 0.0f, 0.0f, -1.0f}, {}, {}, {}});
        const Aabb b = meshBounds(m);
        CHECK_NEAR(b.min.x, -2.0f);
        CHECK_NEAR(b.min.y, -4.0f);
        CHECK_NEAR(b.min.z, -1.0f);
        CHECK_NEAR(b.max.x, 3.0f);
        CHECK_NEAR(b.max.y, 1.0f);
        CHECK_NEAR(b.max.z, 5.0f);
    }
    return iron_test_result();
}
