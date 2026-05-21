#include "test_framework.h"
#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cstdint>

using namespace iron;

int main() {
    // appendBox into an empty mesh yields 24 vertices and 36 indices.
    {
        MeshData m;
        appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f});
        CHECK(m.vertices.size() == 24);
        CHECK(m.indices.size() == 36);
    }

    // Appending a second box accumulates; its indices reference its own
    // vertices (offset past the first box) and stay in range.
    {
        MeshData m;
        appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
        appendBox(m, Vec3{10.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
        CHECK(m.vertices.size() == 48);
        CHECK(m.indices.size() == 72);
        std::uint32_t maxIndex = 0;
        for (std::uint32_t i : m.indices) {
            if (i > maxIndex) maxIndex = i;
        }
        CHECK(maxIndex == 47);
    }

    // A box spans center +/- size/2.
    {
        MeshData m;
        appendBox(m, Vec3{5.0f, 0.0f, 0.0f}, Vec3{4.0f, 2.0f, 2.0f});
        float minX = 1e30f;
        float maxX = -1e30f;
        for (const Vertex& v : m.vertices) {
            if (v.position.x < minX) minX = v.position.x;
            if (v.position.x > maxX) maxX = v.position.x;
        }
        CHECK_NEAR(minX, 3.0f);
        CHECK_NEAR(maxX, 7.0f);
    }

    return iron_test_result();
}
