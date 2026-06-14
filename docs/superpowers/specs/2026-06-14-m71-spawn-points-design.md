# M71 Spawn Points + Runtime Spawning + World-Space Nodes — Design Spec

**Date:** 2026-06-14
**Status:** Approved
**Author:** Elias Hanken (with Claude)
**Roadmap:** `docs/superpowers/2026-06-14-m70-m73-roadmap.md`
**Builds on:** M70 prefabs (`Prefab`, `instantiatePrefab`, `loadPrefabFile`), M69 hierarchy (`worldMatrix`, `Parent`), M68 components-in-graphs, M67 component model.

## Purpose

Give logic graphs the ability to **spawn prefabs into the live game at runtime**,
at **spawn-point markers** placed in the level, and to read/write entity
positions in **world space**. These are the gameplay primitives the
wave-survival arena (and later the class shooter) need for placing players,
enemies, and pickups.

Design model (decided): **passive markers + nodes.** A `SpawnPoint` component is
a tagged location marker with no behavior of its own; spawning is expressed in
logic graphs via new nodes (`SpawnPrefab`, `GetSpawnPoint`/`GetRandomSpawnPoint`).
Game rules (timing, waves, conditions) live in the graph, not in component
fields.

## Goals

- World-space transform nodes `GetWorldPosition` / `SetWorldPosition` (the
  setter converts world→local through the parent chain).
- A `SpawnPoint` marker component (`group` tag + `enabled`), authored and
  serialized like every other component, located by its Transform.
- Spawn-point query nodes returning a marker's **world** position by group.
- A `SpawnPrefab` node that requests a runtime instantiation of a prefab at a
  world position — without engine nodes touching file IO or the renderer.
- Runtime-spawned entities are **ephemeral** (discarded on Stop), reusing the
  existing Play→Stop snapshot; no new teardown code.
- All node/component logic headless and unit-tested; the host seam (queue drain)
  reuses the M70 instantiate path.

## Non-Goals (YAGNI)

- No persistent runtime spawns (spawns do not write back to the authored scene).
- No spawn rotation/scale control yet — a spawn places the prefab root at a
  world **position**; the prefab's authored root rotation/scale are kept. (Full
  transform spawning can come when a game needs facing-direction.)
- No built-in wave/scheduler system — wave pacing is authored in a logic graph
  using a timer + these nodes. (The arena game assembles it.)
- No pooling/recycling of spawned entities (spawn = fresh instantiate).
- No networked spawning (single-player; replication is a later milestone).
- No enemy AI/behavior — that is M72/M73.

## Architecture

Three additive layers — engine nodes/component (pure, tested), a small host seam,
and a data-carrying extension to the node execution context.

### 1. World-space transform nodes — `engine/gameplay/GameplayNodes.cpp`

Two nodes added next to the existing local-space `GetPosition`/`SetPosition`:

- `GetWorldPosition` → out `pos: Vec3`. Reads `worldMatrix(*world, self)` and
  returns its translation column.
- `SetWorldPosition` ← in `pos: Vec3`. For a root entity (no `Parent` or
  `kEntityNone` parent), local == world: `t->position = pos`. For a parented
  entity: `t->position = (inverse(worldMatrix(*world, parent)) * vec4(pos,1)).xyz`.
  Scale-shear caveat under non-uniform parent scale is the standard engine
  caveat (same as `reparentKeepWorld`).

Uses `worldMatrix` from `engine/world/WorldHierarchy.h`, `inverse` from
`engine/math/Mat4.h`, and `Parent` from `engine/world/Parent.h`. Reuses the
existing `gameOf(c)` / `selfTransform(c)` helpers.

### 2. `SpawnPoint` component — `engine/gameplay/SpawnPoint.{h,reflect.cpp}`

```cpp
struct SpawnPoint {
    std::string group;          // tag, e.g. "enemy", "player", "pickup"
    bool        enabled = true; // skipped by queries when false
};
```

Registered the standard way: `registerSpawnPoint(Reflection&)` in
`SpawnPoint.reflect.cpp`; `cr.registerComponent<SpawnPoint>(...)` added to
`engine/scene/RegisterCoreComponents.cpp`; the reflection registration added to
the host's and tests' registration lists (mirrors `registerHealth`). It rides
`ComponentSet`/`EntityJson` for serialization and appears in the
reflection-driven Inspector automatically. The marker has **no behavior** — its
world location is its entity's Transform, read via `worldMatrix`.

### 3. Execution-context extension — `engine/gameplay/GameContext.h`

`GameContext` (handed to every node each tick) gains two host-owned, nullable
fields, so nodes can request spawns and use randomness without depending on the
host or the renderer:

```cpp
struct SpawnRequest { std::string prefabPath; Vec3 position; };

struct GameContext {
    World*    world = nullptr;
    EntityId  self = {};
    float     time = 0.0f;
    float     deltaTime = 0.0f;
    std::vector<SpawnRequest>* spawnQueue = nullptr;  // M71: host drains after tick
    std::uint32_t*            rngState   = nullptr;   // M71: host-seeded RNG state
};
```

A tiny free function `nextRandomU32(std::uint32_t& state)` (xorshift) in the
gameplay layer advances `rngState`; nodes use it for random selection. The host
owns the state (seeded at Play start) so a Play session is reproducible per seed
and nodes stay pure.

### 4. Spawn nodes — `engine/gameplay/GameplayNodes.cpp`

- `GetSpawnPoint` ← in `group: String` → out `pos: Vec3`, `found: Bool`.
  Authored components are mirrored into the World as a single `ComponentSet` per
  entity (M68's `spawnRuntime`, `world.add<ComponentSet>(...)`), so the node
  iterates `world->view<ComponentSet>()` and reads `cs.get<SpawnPoint>()` (NOT a
  `view<SpawnPoint>()`), skips `!enabled` and non-matching `group`, and returns
  the **first** match's world position (`worldMatrix` translation) with
  `found=true`, else `{0,0,0}, found=false`.
- `GetRandomSpawnPoint` — same, but collects all matches and picks one via
  `nextRandomU32(*rngState)` (falls back to first if `rngState` is null).
- `SpawnPrefab` ← exec `in`, `prefab: String`, `pos: Vec3` → exec `then`.
  Pushes `SpawnRequest{prefab, pos}` onto `*spawnQueue` if non-null, then fires
  `then`. Does **no** loading/instantiation itself.

(Port types `String`/`Bool` are used as the existing node system provides them;
the implementation plan confirms exact `PortType` names against `NodeContext.h`.
If a string *port* is unavailable, `prefab`/`group` become node string
properties — same authoring result.)

### 5. Host seam — `games/11-sandbox/main.cpp`

- Declare a `std::vector<iron::SpawnRequest> spawnQueue;` and a
  `std::uint32_t spawnRng;` (seeded at Play start in `togglePlayMode`/`spawnRuntime`).
- When constructing the per-tick context for `tickLogicGraphs`, point the new
  `GameContext` fields at `spawnQueue` / `spawnRng`. (If `tickLogicGraphs`
  builds the `GameContext` internally, extend its signature to accept the queue
  + rng pointers and thread them in — a small, localized change in
  `LogicRuntime`.)
- **After** `tickLogicGraphs` each Play frame, drain `spawnQueue`: for each
  request, run the M70 instantiate path — `loadPrefabFile` (through a small
  path→Prefab cache to avoid re-reading), build `placement` from the prefab
  root transform with `position = request.position`, `instantiatePrefab`, then
  the existing append-World-entities loop (`resolveEntity` + `world.add<...>` +
  `sceneIndexToEntity.push_back`), plus `world.add<ComponentSet>` for each
  spawned entity (matching `spawnRuntime` so the spawned prefab's components are
  visible to nodes), and `mirrorParents()`. Clear the queue.

  Note: spawned entities render and carry their `ComponentSet`, but their own
  `LogicGraph` is **not** activated in M71 (no enemy behavior yet). Activating a
  spawned entity's logic graph — factoring the `LogicGraph` parse out of
  `spawnRuntime` into a shared helper the drain also calls — is deferred to M73
  (enemy AI), which is when spawned entities first need to think.

### 6. Lifecycle

Runtime spawns append to `scene.entities` + World during Play exactly like an
editor instantiate. On Stop, the existing snapshot restore
(`scene = editScene; world = editWorld`) discards them. No new cleanup code; the
authored scene is untouched.

## Data flow (one Play frame)

```
physics.step  →  scene→World transform mirror  →  tickLogicGraphs
   (a SpawnPrefab node pushes SpawnRequest onto spawnQueue)
→  host drains spawnQueue:
     loadPrefabFile (cached) → instantiatePrefab(scene, prefab, {pos})
     → append World entities (resolve + components) → mirrorParents
→  render submit (reads World)
On Stop: snapshot restore discards spawned entities.
```

## Error handling

- `SpawnPrefab` with a null `spawnQueue` (e.g. a graph Run outside Play): the
  node fires `then` and drops the request (logged once at debug level). No crash.
- Drain with a bad/missing `prefabPath`: `loadPrefabFile` returns nullopt →
  host logs `Log::error` and skips that request; other requests still spawn.
- `GetSpawnPoint`/`GetRandomSpawnPoint` with no matching markers: `found=false`,
  `pos={0,0,0}`; a graph can branch on `found`.
- `SetWorldPosition` on an entity with no Transform: no-op, fires `then`.
- Spawn-rate safety: the drain processes at most a host-defined cap per frame
  (e.g. 64) and `log()`s if it truncates, so a runaway graph can't hang a frame.

## Testing strategy

Headless unit tests (CTest, `tests/test_framework.h`):

`tests/test_world_space_nodes.cpp` (or extend `test_gameplay_nodes.cpp`):
- `GetWorldPosition` returns the composed world translation for a parented
  entity (parent at +5, child local +2 → world 7).
- `SetWorldPosition` on a parented entity stores the correct local
  (`inverse(parentWorld)·world`), and on a root stores world verbatim.

`tests/test_spawn_nodes.cpp`:
- `GetSpawnPoint` returns the world position of the first enabled marker in a
  group and `found=true`; `found=false` when none match or all disabled.
- `GetRandomSpawnPoint` returns one of the group's markers (seeded rng →
  deterministic pick asserted).
- `SpawnPrefab` pushes exactly one `SpawnRequest{prefab,pos}` onto the queue and
  fires `then`; with a null queue it fires `then` and pushes nothing.

`tests/test_spawn_point_io.cpp` (or extend `test_scene_io`):
- A `SpawnPoint` component round-trips through scene/prefab IO (group + enabled).

Demo gate (user-run): place two `SpawnPoint` markers tagged `"enemy"`; author a
controller logic graph (timer → `GetRandomSpawnPoint("enemy")` →
`SpawnPrefab(<enemyPrefab>, pos)`); Play → prefabs appear at the markers over
time; Stop → all spawned entities are gone and the scene is unchanged; reload →
only the two markers remain.

## File summary

**Create:**
- `engine/gameplay/SpawnPoint.h`, `engine/gameplay/SpawnPoint.reflect.cpp`
- `tests/test_spawn_nodes.cpp`, `tests/test_world_space_nodes.cpp` (or extend
  `test_gameplay_nodes.cpp`), `tests/test_spawn_point_io.cpp` (or extend
  `test_scene_io.cpp`)

**Modify:**
- `engine/gameplay/GameContext.h` — `SpawnRequest` + the two `GameContext` fields.
- `engine/gameplay/GameplayNodes.cpp` — the 5 new nodes + `nextRandomU32`.
- `engine/gameplay/LogicRuntime.{h,cpp}` — thread the queue + rng into the
  per-entity `GameContext` (signature extension).
- `engine/scene/RegisterCoreComponents.cpp` + the reflection registration sites
  (sandbox `main.cpp`, test registries) — register `SpawnPoint`.
- `engine/CMakeLists.txt` — add `gameplay/SpawnPoint.reflect.cpp`.
- `tests/CMakeLists.txt` — register the new tests.
- `games/11-sandbox/main.cpp` — spawn queue + rng, thread into `tickLogicGraphs`,
  drain-and-instantiate after the tick.
- `docs/engine/` node reference — document the new nodes + `SpawnPoint`.

## Open questions

None. Ready for implementation planning.
