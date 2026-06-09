# M68 — Components in Node Graphs (Design)

**Date:** 2026-06-09
**Status:** Approved
**Kickoff:** `docs/superpowers/m68-kickoff.md`
**Depends on:** M67 extensible component model (merged, PR #100), M53–M56 node system, M55 gameplay runtime.

## Goal

Make entity components usable inside the node graph — the Unreal Blueprint
pattern: read and write a component's reflected fields as graph nodes, so
gameplay logic authored in the Node Editor can e.g. read a Health component
or set an AudioEmitter's gain. Self-entity only; runtime-World writes only.

## Key architectural finding

`NodeTypeDesc.ports` is **static per node type**. A single generic
"Get Component Field" node with component/field dropdowns would need
per-instance dynamic ports — a change rippling through `Graph`, the
evaluator, `NodeGraphPanel`, and JSON IO. Instead, M68 **auto-generates
ordinary node types from the ComponentRegistry** at startup. Generated nodes
are plain `NodeTypeDesc`s, so the searchable create menu (M61), pin
rendering (M56), `catalogToJson` (the AI contract), and graph JSON IO all
work with **zero node-core changes**.

## Decisions (resolved from the kickoff's open questions)

1. **Node shape:** per-component `Get <Component>` (all supported fields as
   output pins, Break-style, plus a `has` Bool) + per-field
   `Set <Component> <field>` (a whole-component Set would stomp unconnected
   fields with defaults). Category `"Components"`. The Get node's subtitle
   lists its field names so menu search for "gain" finds "Get AudioEmitter".
2. **Entity addressing:** self only, via the existing `GameContext`
   (`world->get<ComponentSet>(self)`). "Get component on another entity"
   needs an entity-ref NodeValue — deferred (roadmap M71).
3. **Type bridging (TypeId → PortType):** Float→Float, Int32→Int, Bool→Bool,
   Vec3→Vec3, String→String. Any other TypeId (Quat, Enum, OptionalEnum,
   UInt8, UInt32, Unknown) gets **no pin and no Set node** — skipped, never
   an error.
4. **Read vs write:** new `FieldMeta.readOnly` flag (sibling of `hidden`).
   readOnly → Get pin only, no Set node. `hidden` → no nodes at all.
   `LogicGraphComponent::graph` is marked readOnly (a graph must not rewrite
   its own serialized source mid-run).
5. **Where Set writes:** the runtime World's `ComponentSet` copy during
   Play. Stop's existing M41 snapshot restore (`world = editWorld`) discards
   all graph writes. Authored scene data is never touched; no persist-back.

## Architecture

New engine surface (one file pair), one reflection flag, one demo
component, two lines of host wiring.

### `engine/gameplay/ComponentNodes.{h,cpp}`

```cpp
// Auto-generate "Get <Component>" / "Set <Component> <field>" node types
// from every ComponentRegistry entry. Call once at startup, after
// registerCoreComponents and registerGameplayNodes.
void registerComponentNodes(NodeRegistry& nodes, const ComponentRegistry& components);
```

For each `ComponentRegistry::Entry`:

- **`Get <Name>`** — ports: `has` (Bool, Out) + one Out pin per supported,
  non-hidden field (named after the field). NodeFn: resolve the
  ComponentSet (see Data flow), locate the box by `typeId` via
  `ComponentSet::all()`, read each field through `FieldDesc::ptr<T>` with a
  TypeId switch, emit `NodeValue`s. Missing component / null context →
  `has=false` + `zeroValue(portType)` for every field pin.
- **`Set <Name> <field>`** — one node per supported, non-hidden,
  non-readOnly field. Ports: `in` (Exec, In), `value` (typed, In), `then`
  (Exec, Out). NodeFn: resolve, write via `FieldDesc::ptr<T>`; missing
  component / null context → silent no-op; **always** `fire("then")` so
  exec chains never stall (M55 null-guard convention).

Generation is registration-time only: the lambdas capture the component
`typeId` and copies of the needed `FieldDesc`s (small PODs; their
`string_view` names point at string literals with static storage — safe).
Node `typeName`s contain spaces ("Get Health", "Set Health current") —
typeName is an arbitrary string key everywhere (registry map, graph JSON,
create menu), no parsing anywhere.

### `FieldMeta.readOnly` (engine/reflection/FieldDesc.h)

```cpp
bool readOnly = false;  // Set-node generation skips; Inspector may grey out later
```

Consumed by ComponentNodes in M68. Inspector greying is a noted follow-up,
not in scope. `LogicGraphComponent`'s reflect sidecar marks `graph`
readOnly.

### `engine/gameplay/Health.{h}` + `Health.reflect.cpp` (demo component)

```cpp
struct Health { float current = 100.0f; float max = 100.0f; };
```

Standard-layout POD, reflect sidecar (`registerHealth(Reflection&)`),
registered in `engine/scene/RegisterCoreComponents.cpp` alongside the M67
four. Proves the "one registration call" promise end-to-end: registering it
makes it appear in the Inspector's Add Component menu, SceneIO, AND the
node graph create menu with no further code.

## Data flow (runtime resolution)

- **Edit→Play** (`games/11-sandbox` `spawnRuntime`): for each scene entity,
  `world.add<ComponentSet>(entityId, e.components)` — a deep copy of the
  authored bag becomes a regular World component. `ComponentSet` is already
  copyable (M67) and `World`'s typed arrays already clone it on snapshot
  (M41), so Play/Stop semantics come for free.
- **Node evaluate**: `GameContext` (UNCHANGED — `World* + self` suffice) →
  `world->get<ComponentSet>(self)` → linear scan of `all()` for the
  matching `typeId` → field read/write by offset.
- **Per-frame ordering**: unchanged. The scene→World mirror only writes
  Transform/MeshRef/MaterialDef, so it cannot clobber the runtime
  ComponentSet; `tickLogicGraphs` keeps running after the mirror as in M55.
- **Stop**: existing snapshot restore discards everything.

## Error handling

| Condition | Get | Set |
|---|---|---|
| Null `domainContext` (editor Run preview) | `has=false`, zero pins | no-op, fires `then` |
| Dead entity / no ComponentSet on entity | `has=false`, zero pins | no-op, fires `then` |
| Entity lacks this component | `has=false`, zero pins | no-op, fires `then` |
| Field TypeId unsupported | no pin generated | no node generated |

The TypeId→PortType switch has an explicit skip-default so a future TypeId
can never generate a mistyped pin.

## Demo (visual gate)

Sandbox scene entity with a Health component + seeded logic graph, built
entirely from existing + generated nodes:

```
OnTick ─exec→ Set Health current ─exec→ SetPosition
         dt → Mul(−10) → Add(Get Health current) ──↑ value
         Get Health current → MakeVec3(x, current·0.05, z) ──↑ pos
```

In Play the cube visibly sinks as its health drains; Stop restores it.
Gate also exercises the create menu (search "health"), pin rendering, and
graph JSON round-trip through the Node Editor's Save/Load.

## Testing

- **`tests/test_component_nodes.cpp`** (new):
  - Register a dummy component with one field per supported type
    (Float/Int/Bool/Vec3/String) + a readOnly field + a hidden field + an
    unsupported (Quat) field. Assert: Get node exists with exactly the
    expected pins (+`has`); Set nodes exist for exactly the writable
    supported fields; hidden/Quat get nothing; readOnly gets a Get pin but
    no Set node.
  - Set-then-Get round-trip through a real `World` + `ComponentSet` +
    `GameContext` via `GraphEvaluator` for every supported type.
  - Missing-component path: `has=false`, zero outputs, Set no-op, exec
    continues. Null-domainContext path: same.
  - Generated nodes appear in `catalogToJson`.
- Reflection test grows a `readOnly` round-trip assertion.
- Health registration covered implicitly (it's a registry entry; the
  existing registry/SceneIO tests patterns apply if needed).
- All existing tests stay green (node core, ComponentRegistry, SceneIO
  untouched).

## Out of scope (deferred)

- Entity refs / "get component on another entity" (roadmap M71).
- Enum/Quat/UInt pin support.
- Persist-Play-changes-on-Stop.
- Inspector greying of readOnly fields (cheap follow-up).
- Dynamic per-instance node ports.
- Pushing runtime component values into live systems (e.g. AudioEmitter
  gain → playing voice) — the demo deliberately uses Transform, which is
  already live.

## Process

Standard superpowers flow: this spec → writing-plans →
subagent-driven-development → visual gate → PR + squash-merge + memory
update. Branch `m68-components-in-node-graphs` off `main`. Canonical build
dir `build-vk` (ctest needs `-C Debug`); verify build exit codes, not
truncated tails.
