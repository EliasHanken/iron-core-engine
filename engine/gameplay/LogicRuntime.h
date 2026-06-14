#pragma once

#include "gameplay/GameContext.h"

#include <cstdint>
#include <vector>

namespace iron {

class World;
class NodeRegistry;

// Run every entity's LogicGraph once. For each, builds a GameContext{world,
// self, time, deltaTime} + a RunContext (persistent vars from the component)
// and evaluates the graph; nodes read/write the entity via GetPosition/etc.
//
// Tick every entity's LogicGraph. `spawnQueue` / `rngState` (M71) are forwarded
// into each entity's GameContext; pass nullptr (the defaults) when spawning /
// randomness are not needed (headless tests, non-Play ticks).
void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime,
                     std::vector<SpawnRequest>* spawnQueue = nullptr,
                     std::uint32_t* rngState = nullptr);

}  // namespace iron
