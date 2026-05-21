#include "scene/Mesh.h"

#include <cstdint>

namespace iron {

void appendBox(MeshData& out, Vec3 center, Vec3 size) {
    // Six faces, each a quad of 4 vertices with a shared outward normal.
    // Corner components are +/-0.5 (a unit cube); scaled by `size` and
    // shifted by `center` they span center +/- size/2. Winding is
    // counter-clockwise seen from outside.
    struct Face {
        Vec3 normal;
        Vec3 corners[4];
    };

    const Face faces[6] = {
        {{ 1, 0, 0}, {{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f}}},
        {{-1, 0, 0}, {{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 1, 0}, {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}}},
        {{ 0,-1, 0}, {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0, 0,-1}, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}}},
    };

    const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (int i = 0; i < 4; ++i) {
            const Vec3 c = face.corners[i];
            const Vec3 position{
                center.x + c.x * size.x,
                center.y + c.y * size.y,
                center.z + c.z * size.z,
            };
            out.vertices.push_back(Vertex{position, face.normal, uvs[i]});
        }
        // Two triangles per quad: (0,1,2) and (0,2,3).
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
    }
}

// A unit cube centered at the origin (side length 1) — built from appendBox.
MeshData makeCube() {
    MeshData data;
    appendBox(data, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    return data;
}

} // namespace iron
