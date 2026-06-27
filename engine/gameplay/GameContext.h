#pragma once

#include "math/Vec.h"
#include "world/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

class World;

// M71: a queued request to instantiate a prefab at a world position. The
// SpawnPrefab node pushes these; the host drains them after the logic tick and
// runs the M70 instantiate path.
struct SpawnRequest {
    std::string prefabPath;
    Vec3        position;
};

// The gameplay-domain context a logic graph runs against. Set into
// RunContext::domainContext (as a void*) by the runtime each tick.
struct GameContext {
    World*   world = nullptr;
    EntityId self = {};        // the entity owning the running graph
    float    time = 0.0f;      // elapsed Play seconds
    float    deltaTime = 0.0f;
    // M71: host-owned, nullable. Nodes use these without depending on the host
    // or the renderer; null when running headless / in the editor preview.
    std::vector<SpawnRequest>* spawnQueue = nullptr;
    std::uint32_t*             rngState   = nullptr;
};

// M71: xorshift32. `state` must be non-zero; advances it and returns the new
// value. Host owns the state (seeded at Play start) so a Play run is
// reproducible per seed and nodes stay pure.
inline std::uint32_t nextRandomU32(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace iron
