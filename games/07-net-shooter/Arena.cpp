#include "Arena.h"

namespace iron::netshooter {

namespace {
// Tiny xorshift32 — deterministic across compilers, no <random>
// platform dependence.
std::uint32_t xorshift32(std::uint32_t& state) {
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

float frand(std::uint32_t& state, float lo, float hi) {
    const float t = (xorshift32(state) & 0xFFFFFF) / static_cast<float>(0x1000000);
    return lo + t * (hi - lo);
}
}  // namespace

Arena buildArena(std::uint32_t seed) {
    Arena a;
    // Floor: 30x30 at y in [-0.5, 0].
    a.boxes.push_back(Aabb{Vec3{-15.0f, -0.5f, -15.0f}, Vec3{15.0f, 0.0f, 15.0f}});
    // Four perimeter walls (3 m tall, 0.5 m thick).
    a.boxes.push_back(Aabb{Vec3{-15.5f, 0.0f, -15.5f}, Vec3{15.5f, 3.0f, -15.0f}});
    a.boxes.push_back(Aabb{Vec3{-15.5f, 0.0f, 15.0f},  Vec3{15.5f, 3.0f, 15.5f}});
    a.boxes.push_back(Aabb{Vec3{-15.5f, 0.0f, -15.0f}, Vec3{-15.0f, 3.0f, 15.0f}});
    a.boxes.push_back(Aabb{Vec3{15.0f, 0.0f, -15.0f},  Vec3{15.5f, 3.0f, 15.0f}});

    // 10 cover boxes, 2x2x2, on a 3-by-3 grid with seeded per-cell
    // offset. (Last grid slot is just dropped — we want 9 grid plus 1
    // central tall block.)
    std::uint32_t rng = seed;
    for (int gx = 0; gx < 3; ++gx) {
        for (int gz = 0; gz < 3; ++gz) {
            const float cellX = -10.0f + gx * 10.0f + frand(rng, -2.0f, 2.0f);
            const float cellZ = -10.0f + gz * 10.0f + frand(rng, -2.0f, 2.0f);
            a.boxes.push_back(Aabb{Vec3{cellX - 1.0f, 0.0f, cellZ - 1.0f},
                                   Vec3{cellX + 1.0f, 2.0f, cellZ + 1.0f}});
        }
    }
    // Central tall pillar — 1x3x1 — for splash-damage cover.
    a.boxes.push_back(Aabb{Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 3.0f, 0.5f}});

    // Four corner spawns, ~1.5 m off the wall.
    a.spawnPoints = {
        Vec3{-13.0f, 1.0f, -13.0f},
        Vec3{ 13.0f, 1.0f, -13.0f},
        Vec3{-13.0f, 1.0f,  13.0f},
        Vec3{ 13.0f, 1.0f,  13.0f},
    };
    return a;
}

Vec3 pickRandomSpawn(const Arena& arena, std::uint32_t& rngState) {
    if (arena.spawnPoints.empty()) return Vec3{0.0f, 1.0f, 0.0f};
    const auto idx = xorshift32(rngState) % arena.spawnPoints.size();
    return arena.spawnPoints[idx];
}

}  // namespace iron::netshooter
