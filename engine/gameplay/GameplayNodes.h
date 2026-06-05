#pragma once

namespace iron {
class NodeRegistry;
// Register the world-aware node set: OnTick, GetPosition, SetPosition,
// Translate, MakeVec3, BreakVec3, Mul, Sin, GetVar, SetVar.
void registerGameplayNodes(NodeRegistry& registry);
}  // namespace iron
