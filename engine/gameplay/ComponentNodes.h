#pragma once

namespace iron {

class NodeRegistry;
class ComponentRegistry;

// M68: auto-generate component-field node types from every ComponentRegistry
// entry (category "Components"):
//   - "Get <Component>": a `has` Bool output + one output pin per supported,
//     non-hidden field (Blueprint Break-style).
//   - "Set <Component> <field>": one node per supported, non-hidden,
//     non-readOnly field (exec in, typed `value` in, exec `then` out).
// Supported TypeIds: Float, Int32, Bool, Vec3, String — anything else gets no
// pin and no Set node. A field literally named "has" is skipped (reserved for
// the Get node's has pin). Nodes resolve self-only via GameContext against
// world->get<ComponentSet>(self); missing component / null context => Get
// reports has=false + zeros, Set is a silent no-op, exec always continues.
// NOTE: data outputs are memoized per run at first pull — a Get node pulled
// before a Set in the same tick keeps its pre-write values; place a separate
// Get node after the Set to re-read.
// Call once at startup, after registerCoreComponents + registerGameplayNodes.
void registerComponentNodes(NodeRegistry& nodes, const ComponentRegistry& components);

}  // namespace iron
