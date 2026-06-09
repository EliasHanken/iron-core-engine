# M68 Kickoff — Components usable in node graphs

Handoff note for starting M68 in a fresh session. Read this, then run the
**superpowers:brainstorming** skill to turn it into a design → spec → plan.

## Where we are (just landed)

- **M67 "Extensible Component Model" is DONE + MERGED** (PR #100, squash `5e86f70`).
  `SceneEntity` now holds a registry-driven `ComponentSet` instead of hardcoded
  `std::optional<X>` fields. Adding an authorable component is one registration call.
- The three-milestone **Component + Hierarchy overhaul**: M67 spine (done) →
  **M68 components-in-node-graphs (this)** → M69 scene hierarchy.

## M68 goal

Make entity components **usable inside the node graph** — the Unreal Blueprint
pattern: read and write a component's reflected fields as graph nodes, so gameplay
logic authored in the Node Editor can e.g. read a Health component or set an
AudioEmitter's gain. Everything needed already exists; M68 is mostly *wiring* the
component model into the node system.

## Existing building blocks (the foundation is all here)

- **ComponentRegistry** (`engine/world/ComponentRegistry.h`) — every authorable
  component type: `typeId`, `name`, `std::span<const FieldDesc> fields`, `factory()`.
  `byTypeId/byName/order`.
- **Reflection** (`engine/reflection/Reflection.h`) + **FieldDesc**
  (`engine/reflection/FieldDesc.h`: `name`, `TypeId type`, `offset`, `meta`,
  `enumTypeId`; `ptr<T>(obj)`). Fields are read/written generically by offset+TypeId.
- **ComponentSet** (`engine/world/ComponentSet.h`) — per-entity bag:
  `get<T>()`, `all()`, typed + by-typeId access. Lives on `SceneEntity`.
- **iron::World** (`engine/world/World.h`) — runtime ECS store; the editor mirrors
  the scene into it on Play. LogicGraphs run against World entities.
- **Node system (M53–55)**: `engine/nodes/` (NodeGraph/NodeValue/NodeRegistry/
  GraphEvaluator/JSON IO), `engine/gameplay/` (LogicGraph component, LogicRuntime
  `tickLogicGraphs(World&, NodeRegistry&, time, dt)`, GameContext/domainContext
  bridge), editor `GraphEditorModel` + `NodeGraphPanel`. M55 already has
  OnTick/Transform/var nodes and runs a per-entity graph in Play (the demo entity
  bobs via a graph). `LogicGraphComponent { std::string graph; }` holds the
  serialized graph per entity.
- The node editor's create-menu is searchable + category-grouped (M61); node
  catalog is reflection/registry-style data.

## Open design questions to resolve in the brainstorm

1. **Node shape:** ONE generic registry-driven `Get Component Field` / `Set
   Component Field` node (pick component + field via dropdowns, typed by
   FieldDesc) — vs. **auto-generated** per-component-per-field nodes in the create
   menu (more discoverable, more nodes). Or a hybrid (per-component Get/Set with a
   field selector). Recommendation to weigh: the generic+selector node is least
   code and rides FieldDesc directly; auto-generated nodes feel more Blueprint-y.
2. **Entity addressing:** how does a graph reference "the entity it runs on"? M55's
   GameContext already binds a graph to an entity in Play — confirm/extend that so
   Get/Set resolve against `world.get<T>(self)` (or the ComponentSet). Self-only
   for M68, or also "get component on another entity" (needs an entity ref value —
   probably defer)?
3. **Type bridging:** map `FieldDesc::TypeId` (Float/Int/Bool/Vec3/Quat/Enum/String)
   ↔ `NodeValue` types. Which field types are supported in v1 (likely
   Float/Int/Bool/Vec3; Enum/String later)?
4. **Read vs write:** all reflected fields writable? Honor a `readOnly` FieldMeta
   flag (FieldMeta currently has `hidden` from M67 — a `readOnly` sibling may be
   worth adding)? Write timing (apply during tick → mutate the runtime World
   component; does it persist back to the scene on Stop, like collision does?).
5. **Where Set writes:** runtime `World` component during Play (most likely), and
   how that interacts with the existing Play→Stop snapshot/restore.

## Process

Standard superpowers flow: **brainstorming → writing-plans → subagent-driven
development → visual gate → PR + squash-merge + memory update.** Canonical build
dir `build-vk` (Vulkan, multi-config MSVC — ctest needs `-C Debug`). Verify build
EXIT CODES, not the truncated tail. Commit messages end with the
`Co-Authored-By: Claude Opus 4.8` trailer; PR bodies end with the Claude Code
footer. Per-milestone branch off `main`.
