#pragma once

namespace iron {
class Reflection;
class ComponentRegistry;

// Register the engine's authorable components into `cr`. The corresponding
// Reflection types must already be registered in `r` (registerCollisionShape,
// registerAudioEmitter, registerReflectionProbe, registerLogicGraphComponent).
void registerCoreComponents(ComponentRegistry& cr, const Reflection& r);
}  // namespace iron
