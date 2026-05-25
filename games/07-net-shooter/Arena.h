#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron::netshooter {

// Procedurally-generated FFA arena. Seeded by a constant so host and
// clients see identical geometry — they only render it; the host alone
// raycasts against it.
struct Arena {
    std::vector<Aabb> boxes;        // floor + 4 walls + ~10 cover boxes
    std::vector<Vec3> spawnPoints;  // 4 corners
};

// Build a deterministic arena. `seed` is the RNG seed — call sites pass
// a fixed value.
Arena buildArena(std::uint32_t seed = 0xA5A5);

// Pick a random spawn point using a caller-owned RNG state (so the
// host can call this without affecting the deterministic geometry).
Vec3 pickRandomSpawn(const Arena& arena, std::uint32_t& rngState);

}  // namespace iron::netshooter
