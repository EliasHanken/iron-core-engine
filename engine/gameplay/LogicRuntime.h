#pragma once

namespace iron {

class World;
class NodeRegistry;

// Run every entity's LogicGraph once. For each, builds a GameContext{world,
// self, time, deltaTime} + a RunContext (persistent vars from the component)
// and evaluates the graph; nodes read/write the entity via GetPosition/etc.
void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime);

}  // namespace iron
