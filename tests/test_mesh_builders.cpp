#include "test_framework.h"
#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cmath>
#include <cstdint>
#include <vector>

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

    // appendBox writes world-space-extent UVs so textures tile rather than
    // stretch. For size {4,2,1} the per-face UV maxes are:
    //   +X/-X: (size.z=1, size.y=2)  →  max V contributes 2
    //   +Y/-Y: (size.x=4, size.z=1)  →  max U contributes 4
    //   +Z/-Z: (size.x=4, size.y=2)  →  max U=4, max V=2
    // So the largest U across all 24 verts is 4, largest V is 2.
    {
        MeshData m;
        appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 2.0f, 1.0f});
        float maxU = 0.0f, maxV = 0.0f;
        for (const auto& v : m.vertices) {
            if (v.uv.x > maxU) maxU = v.uv.x;
            if (v.uv.y > maxV) maxV = v.uv.y;
        }
        CHECK_NEAR(maxU, 4.0f);
        CHECK_NEAR(maxV, 2.0f);
    }

    // appendTube over 3 points with 6 sides: 3*(6+1) = 21 vertices (each ring
    // has a duplicated seam vertex), (3-1)*6*6 = 72 indices.
    {
        MeshData m;
        std::vector<Vec3> pts = {
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f},
            Vec3{0.0f, 0.0f, -2.0f},
        };
        appendTube(m, pts, 0.5f, 6);
        CHECK(m.vertices.size() == 21);
        CHECK(m.indices.size() == 72);
    }

    // Tube ring vertices sit at `radius` from the polyline; normals are unit
    // length and point radially outward.
    {
        MeshData m;
        std::vector<Vec3> pts = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -4.0f}};
        appendTube(m, pts, 2.0f, 8);
        for (const Vertex& v : m.vertices) {
            // The polyline runs along Z, so distance from the Z axis is the radius.
            CHECK_NEAR(std::sqrt(v.position.x * v.position.x
                               + v.position.y * v.position.y), 2.0f);
            CHECK_NEAR(std::sqrt(v.normal.x * v.normal.x
                               + v.normal.y * v.normal.y
                               + v.normal.z * v.normal.z), 1.0f);
        }
    }

    // Degenerate inputs append nothing: fewer than 2 points, fewer than 3
    // sides, or a non-positive radius.
    {
        MeshData m;
        std::vector<Vec3> one = {Vec3{0.0f, 0.0f, 0.0f}};
        appendTube(m, one, 1.0f, 6);
        CHECK(m.vertices.empty());
        CHECK(m.indices.empty());

        std::vector<Vec3> two = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        appendTube(m, two, 1.0f, 2);     // too few sides
        CHECK(m.vertices.empty());
        appendTube(m, two, 0.0f, 6);     // non-positive radius
        CHECK(m.vertices.empty());
    }

    // --- appendQuad ---
    // 1. Single quad: 4 vertices, 6 indices.
    {
        MeshData m;
        appendQuad(m, Vec3{0.0f, 0.0f, 0.0f}, Vec2{2.0f, 2.0f},
                   Vec3{0.0f, 1.0f, 0.0f});
        CHECK(m.vertices.size() == 4u);
        CHECK(m.indices.size() == 6u);
    }

    // 2. Index offsets accumulate when appending to a non-empty mesh.
    {
        MeshData m;
        appendQuad(m, Vec3{0.0f, 0.0f, 0.0f}, Vec2{2.0f, 2.0f},
                   Vec3{0.0f, 1.0f, 0.0f});
        appendQuad(m, Vec3{10.0f, 0.0f, 0.0f}, Vec2{2.0f, 2.0f},
                   Vec3{0.0f, 1.0f, 0.0f});
        CHECK(m.vertices.size() == 8u);
        CHECK(m.indices.size() == 12u);
        // The second quad's indices should all be in [4, 7].
        for (std::size_t i = 6; i < 12; ++i) {
            CHECK(m.indices[i] >= 4u);
            CHECK(m.indices[i] <= 7u);
        }
    }

    // 3. Spatial extent for normal={0,1,0}, size={4,2}: x spans +-2, y=0,
    //    z spans +-1 (since v=-Z, size.y stretches symmetrically in Z).
    {
        MeshData m;
        appendQuad(m, Vec3{0.0f, 0.0f, 0.0f}, Vec2{4.0f, 2.0f},
                   Vec3{0.0f, 1.0f, 0.0f});
        float minX = 1e9f, maxX = -1e9f;
        float minZ = 1e9f, maxZ = -1e9f;
        for (const auto& v : m.vertices) {
            CHECK_NEAR(v.position.y, 0.0f);
            if (v.position.x < minX) minX = v.position.x;
            if (v.position.x > maxX) maxX = v.position.x;
            if (v.position.z < minZ) minZ = v.position.z;
            if (v.position.z > maxZ) maxZ = v.position.z;
        }
        CHECK_NEAR(minX, -2.0f);
        CHECK_NEAR(maxX,  2.0f);
        CHECK_NEAR(minZ, -1.0f);
        CHECK_NEAR(maxZ,  1.0f);
    }

    // 4. All vertices share the input normal.
    {
        MeshData m;
        Vec3 normal{0.0f, 1.0f, 0.0f};
        appendQuad(m, Vec3{0.0f, 0.0f, 0.0f}, Vec2{2.0f, 2.0f}, normal);
        for (const auto& v : m.vertices) {
            CHECK_NEAR(v.normal.x, normal.x);
            CHECK_NEAR(v.normal.y, normal.y);
            CHECK_NEAR(v.normal.z, normal.z);
        }
    }

    return iron_test_result();
}
