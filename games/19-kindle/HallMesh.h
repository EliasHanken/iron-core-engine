#pragma once

// Builds the dead machine as a single mesh, from code. Everything stays inside the
// margin that Swarm keeps from Room's walls and pillars, so collision can stay the
// plain math of the Room, and only the look gets more complicated.

#include "Swarm.h"

#include "scene/Mesh.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <tuple>

namespace kindle {

namespace hall {

using iron::MeshData;

// Small and repeatable, so the hall is the same on every run.
class Random {
public:
    float next() {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<float>(state_ >> 8) / 16777216.0f;
    }
    float range(float low, float high) { return low + (high - low) * next(); }

private:
    std::uint32_t state_ = 41u;
};

inline void appendCylinder(MeshData& out, Vec3 a, Vec3 b, float radius, int sides) {
    const Vec3 axis = iron::normalize(b - a);
    const Vec3 helper = std::abs(axis.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 side = iron::normalize(iron::cross(axis, helper));
    const Vec3 up = iron::cross(side, axis);

    const auto first = static_cast<std::uint32_t>(out.vertices.size());
    for (int i = 0; i <= sides; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(sides);
        const Vec3 n = side * std::cos(angle) + up * std::sin(angle);
        const float u = static_cast<float>(i) / static_cast<float>(sides);
        out.vertices.push_back({a + n * radius, n, {u, 0.0f}, axis});
        out.vertices.push_back({b + n * radius, n, {u, 1.0f}, axis});
    }
    for (int i = 0; i < sides; ++i) {
        const std::uint32_t v = first + static_cast<std::uint32_t>(i) * 2;
        out.indices.insert(out.indices.end(), {v, v + 2, v + 1, v + 1, v + 2, v + 3});
    }
}

inline void addShell(MeshData& out, const Room& room) {
    const Vec3 h = room.half;
    constexpr float t = 1.0f;
    iron::appendBox(out, {0, -h.y - 0.15f - t, 0}, {h.x * 2 + 4, t * 2, h.z * 2 + 4});
    iron::appendBox(out, {0, h.y + t, 0}, {h.x * 2 + 4, t * 2, h.z * 2 + 4});
    iron::appendBox(out, {-h.x - t, 0, 0}, {t * 2, h.y * 2, h.z * 2 + 4});
    iron::appendBox(out, {h.x + t, 0, 0}, {t * 2, h.y * 2, h.z * 2 + 4});
    iron::appendBox(out, {0, 0, -h.z - t}, {h.x * 2, h.y * 2, t * 2});
    iron::appendBox(out, {0, 0, h.z + t}, {h.x * 2, h.y * 2, t * 2});
}

// Floor plates with real gaps between them, which catch the light as you pass.
inline void addFloor(MeshData& out, const Room& room) {
    constexpr float plate = 2.5f;
    constexpr float gap = 0.05f;
    for (float x = -room.half.x; x < room.half.x; x += plate) {
        for (float z = -room.half.z; z < room.half.z; z += plate) {
            iron::appendBox(out, {x + plate * 0.5f, -room.half.y - 0.08f, z + plate * 0.5f},
                            {plate - gap, 0.16f, plate - gap});
        }
    }
}

inline void addPillar(MeshData& out, const Room& room, float x, float z) {
    const float w = room.pillarHalf;
    const float floorY = -room.half.y;
    const float ceilingY = room.half.y;

    iron::appendBox(out, {x, 0, z}, {w * 2, room.half.y * 2, w * 2});

    // A stepped foot and head, with collars between them.
    iron::appendBox(out, {x, floorY + 0.3f, z}, {w * 2 + 0.7f, 0.6f, w * 2 + 0.7f});
    iron::appendBox(out, {x, floorY + 0.8f, z}, {w * 2 + 0.35f, 0.4f, w * 2 + 0.35f});
    iron::appendBox(out, {x, ceilingY - 0.3f, z}, {w * 2 + 0.7f, 0.6f, w * 2 + 0.7f});
    iron::appendBox(out, {x, ceilingY - 0.8f, z}, {w * 2 + 0.35f, 0.4f, w * 2 + 0.35f});
    for (const float y : {-2.2f, 2.4f}) {
        iron::appendBox(out, {x, y, z}, {w * 2 + 0.24f, 0.35f, w * 2 + 0.24f});
    }

    // Ribs up each face.
    for (const float along : {-0.5f, 0.0f, 0.5f}) {
        const Vec3 tall{0.12f, room.half.y * 2 - 2.0f, 0.1f};
        iron::appendBox(out, {x + along, 0, z + w + 0.05f}, tall);
        iron::appendBox(out, {x + along, 0, z - w - 0.05f}, tall);
        iron::appendBox(out, {x + w + 0.05f, 0, z + along}, {tall.z, tall.y, tall.x});
        iron::appendBox(out, {x - w - 0.05f, 0, z + along}, {tall.z, tall.y, tall.x});
    }
}

inline void addWalls(MeshData& out, const Room& room) {
    const Vec3 h = room.half;
    for (float along = -h.x + 2.5f; along < h.x; along += 5.0f) {
        const Vec3 rib{0.6f, h.y * 2, 0.5f};
        iron::appendBox(out, {along, 0, -h.z + 0.25f}, rib);
        iron::appendBox(out, {along, 0, h.z - 0.25f}, rib);
        iron::appendBox(out, {-h.x + 0.25f, 0, along}, {rib.z, rib.y, rib.x});
        iron::appendBox(out, {h.x - 0.25f, 0, along}, {rib.z, rib.y, rib.x});
    }
    // A band of pipes round the room, held by brackets.
    for (const auto& [y, radius, inset] : {std::tuple{-1.4f, 0.26f, 0.42f}, std::tuple{-0.75f, 0.16f, 0.34f},
                                           std::tuple{3.6f, 0.2f, 0.36f}}) {
        const float x = h.x - inset;
        const float z = h.z - inset;
        appendCylinder(out, {-x, y, -z}, {x, y, -z}, radius, 12);
        appendCylinder(out, {-x, y, z}, {x, y, z}, radius, 12);
        appendCylinder(out, {-x, y, -z}, {-x, y, z}, radius, 12);
        appendCylinder(out, {x, y, -z}, {x, y, z}, radius, 12);
    }
    // A catwalk along one wall, narrow enough to stay outside the swarm's margin.
    constexpr float kCatwalkY = 1.6f;
    constexpr float kDepth = 0.62f;
    iron::appendBox(out, {-h.x + kDepth * 0.5f, kCatwalkY, 0}, {kDepth, 0.08f, h.z * 2});
    for (float z = -h.z + 1.0f; z < h.z; z += 2.0f) {
        iron::appendBox(out, {-h.x + kDepth, kCatwalkY + 0.5f, z}, {0.05f, 1.0f, 0.05f});
    }
    iron::appendBox(out, {-h.x + kDepth, kCatwalkY + 1.0f, 0}, {0.06f, 0.06f, h.z * 2});
    iron::appendBox(out, {-h.x + kDepth, kCatwalkY + 0.5f, 0}, {0.04f, 0.04f, h.z * 2});
}

inline void addCeiling(MeshData& out, const Room& room) {
    const Vec3 h = room.half;
    for (float along = -h.x + 2.5f; along < h.x; along += 5.0f) {
        iron::appendBox(out, {along, h.y - 0.28f, 0}, {0.38f, 0.56f, h.z * 2});
        iron::appendBox(out, {0, h.y - 0.2f, along}, {h.x * 2, 0.4f, 0.3f});
    }
}

// Heavy cables sagging between girders. They hang above the swarm's reach, so
// they only ever pass overhead.
inline void addCables(MeshData& out, const Room& room, Random& random) {
    constexpr int kSegments = 12;
    for (int i = 0; i < 26; ++i) {
        const float spanX = random.range(-17.0f, 17.0f);
        const float spanZ = random.range(-17.0f, 17.0f);
        const float angle = random.range(0.0f, 6.28f);
        const float length = random.range(5.0f, 11.0f);
        const Vec3 start{spanX, room.half.y - 0.5f, spanZ};
        const Vec3 end{spanX + std::cos(angle) * length, room.half.y - 0.5f, spanZ + std::sin(angle) * length};
        const float sag = random.range(0.8f, 2.4f);
        const float radius = random.range(0.03f, 0.07f);

        Vec3 last = start;
        for (int s = 1; s <= kSegments; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(kSegments);
            const Vec3 point = start + (end - start) * t + Vec3{0, -sag * 4.0f * t * (1.0f - t), 0};
            appendCylinder(out, last, point, radius, 6);
            last = point;
        }
    }
}

// Boxes and conduits along the foot of the walls, so nothing looks finished off.
inline void addClutter(MeshData& out, const Room& room, Random& random) {
    const Vec3 h = room.half;
    for (int i = 0; i < 220; ++i) {
        const float width = random.range(0.25f, 1.3f);
        const float height = random.range(0.2f, 1.2f);
        const float depth = random.range(0.12f, 0.46f);
        const float along = random.range(-h.x + 0.5f, h.x - 0.5f);
        const float y = random.range(-h.y + height * 0.5f, 0.0f);
        switch (i % 4) {
            case 0: iron::appendBox(out, {along, y, -h.z + depth * 0.5f}, {width, height, depth}); break;
            case 1: iron::appendBox(out, {along, y, h.z - depth * 0.5f}, {width, height, depth}); break;
            case 2: iron::appendBox(out, {-h.x + depth * 0.5f, y, along}, {depth, height, width}); break;
            default: iron::appendBox(out, {h.x - depth * 0.5f, y, along}, {depth, height, width}); break;
        }
    }
}

}  // namespace hall

inline iron::MeshData buildHallMesh(const Room& room) {
    iron::MeshData mesh;
    hall::Random random;
    hall::addShell(mesh, room);
    hall::addFloor(mesh, room);
    for (const float x : {-15.0f, -5.0f, 5.0f, 15.0f}) {
        for (const float z : {-15.0f, -5.0f, 5.0f, 15.0f}) hall::addPillar(mesh, room, x, z);
    }
    hall::addWalls(mesh, room);
    hall::addCeiling(mesh, room);
    hall::addCables(mesh, room, random);
    hall::addClutter(mesh, room, random);
    return mesh;
}

}  // namespace kindle
