#pragma once

#include <string>

namespace iron {

// M71 marker component: a placed, tagged spawn location. No behavior of its own
// — its world location is its entity's Transform. Logic graphs query it by
// `group` via GetSpawnPoint / GetRandomSpawnPoint and spawn prefabs there.
// Reflected like every other component (rides ComponentSet + EntityJson +
// the reflection-driven Inspector with zero bespoke code).
struct SpawnPoint {
    std::string group;          // tag, e.g. "enemy", "player", "pickup"
    bool        enabled = true; // queries skip disabled markers
};

}  // namespace iron
