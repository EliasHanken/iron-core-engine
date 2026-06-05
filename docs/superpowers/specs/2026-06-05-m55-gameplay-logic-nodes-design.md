# M55 — Gameplay Logic Nodes + Runtime — Design

> **Sub-project #3 of the node-system track** (after M53 core + M54 editor). Track: #1 core (merged) → #2 editor (merged) → **#3 gameplay logic nodes + runtime (this)** → #4 shader graph → #5 VFX → #6 UI nodes. This is the "create games with nodes" payoff: a graph attached to an entity runs its behavior in Play mode.

## Goal

Make node graphs **drive entity behavior** in the live ECS during Play: an `OnTick` event + world-aware nodes (read/write the entity's `Transform`) + persistent per-entity variables. Author a graph in the M54 node editor, assign it to a scene entity, hit Play, and watch the entity move — no C++.

## The bridge (key architectural decision)

M53's core stays **domain-agnostic** (so #4 shaders / #5 VFX / #6 UI can reuse it without a `World` dependency). The only core change is **one opaque slot** on `RunContext`:

```cpp
// engine/nodes/NodeContext.h — RunContext gains:
void* domainContext = nullptr;   // each domain attaches its own context here
```

The gameplay module defines the concrete context and sets it; gameplay nodes cast it back. All `World` coupling lives in the gameplay module, never the core.

```cpp
// engine/gameplay/GameContext.h
struct GameContext {
    World*    world = nullptr;
    EntityId  self;            // the entity owning the running graph
    float     time = 0.0f;     // elapsed seconds (Play time)
    float     deltaTime = 0.0f;
};
```

A gameplay node retrieves it via a helper and **null-guards** (so running a gameplay graph in the editor's headless "Run" — where `domainContext == nullptr` — is a safe no-op rather than a crash):

```cpp
GameContext* g = static_cast<GameContext*>(c.run().domainContext);
if (!g || !g->world) return;   // no world (e.g. editor preview) -> no-op
```

## Entry-node generalization (small M53 core touch)

M53's evaluator starts at the first node named `"Entry"`. Gameplay graphs start at `OnTick`. Generalize: add `bool isEntry = false;` to `NodeTypeDesc`; the evaluator starts at the **first node whose type has `isEntry == true`**. M53's `Entry` sets `isEntry = true`; the new `OnTick` sets it too. (v1 has one entry per graph; multiple events are a later refinement.) `test_node_graph`'s existing Entry behavior is preserved.

## Node set (v1)

A gameplay module registers these (in addition to M53's `Entry`/`Const`/`Add`/`Compare`/`Branch`/`Sequence`/`SetOutput`):
- **`OnTick`** — entry event, `isEntry`. Ports: `[out exec "then", out Float "dt", out Float "time"]` (from `GameContext.deltaTime`/`time`).
- **`GetPosition`** — `[out Vec3 "pos"]` — the self entity's `Transform.position` (zero if no Transform).
- **`SetPosition`** — `[in exec "in", in Vec3 "pos"]` — writes `Transform.position`.
- **`Translate`** — `[in exec "in", in Vec3 "delta"]` — `Transform.position += delta`.
- **`MakeVec3`** — `[in Float "x", in Float "y", in Float "z", out Vec3 "v"]`.
- **`BreakVec3`** — `[in Vec3 "v", out Float "x", out Float "y", out Float "z"]`.
- **`Mul`** — `[in Float "a", in Float "b", out Float "result"]`.
- **`Sin`** — `[in Float "x", out Float "result"]`.
- **`GetVar`** — `[in String "name", out Float "value"]` — reads `RunContext.vars[name]` (persistent per entity).
- **`SetVar`** — `[in exec "in", in String "name", in Float "value"]` — writes `RunContext.vars[name]`.

These are **headless-unit-testable**: build a `World`, an entity with a `Transform`, a `RunContext{domainContext=&gameCtx, vars=…}`, run a graph, assert the `Transform` changed.

## The runtime

```cpp
// engine/gameplay/LogicGraph.h — a component on an entity.
struct LogicGraph {
    Graph graph;
    std::unordered_map<std::string, NodeValue> vars;   // persistent across ticks
    bool started = false;                              // reserved for a future OnStart
};

// engine/gameplay/LogicRuntime.h
void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime);
```

`tickLogicGraphs` iterates entities with a `LogicGraph` (`world.view<LogicGraph>()`), and for each builds a `GameContext{&world, entity, time, deltaTime}` and a `RunContext` whose `vars` are the component's persistent `vars` and whose `domainContext` points at the GameContext, then calls `iron::run(lg.graph, registry, ctx)`. After the run, the (possibly mutated) `vars` are kept on the component for the next tick. **Unit-tested**: create a world + entity + `Transform` + a `LogicGraph` (e.g. an `OnTick→Translate` move graph), tick several frames, assert the position advanced and a `SetVar`/`GetVar` value persists.

## Authoring + Play integration (the sandbox)

- **`SceneEntity` gains a logic-graph field** (`engine/scene/SceneFormat.h`): `std::string logicGraph;` — the serialized graph JSON (empty = none). Round-trips through the existing reflection-driven scene IO (a plain string field; no new IO machinery). Stored on the authored scene so it persists in Edit and Save/Load.
- **Node editor "Assign to selected entity"** — the `NodeGraphPanel` adds a button that returns an intent (it already has the model); the host writes `graphModel.toJson().dump()` into the selected `SceneEntity::logicGraph` (panel returns the intent; host performs the write, per the existing panel pattern).
- **scene→World build** — where the sandbox builds the `World` from the scene on Play (it already adds `Transform`/`MeshRef`/…), if `se.logicGraph` is non-empty, parse it with `fromJson` and `world.add<LogicGraph>(entity, {graph, {}, false})`.
- **Play loop** — in the `if (editor.isPlaying())` per-frame section, call `tickLogicGraphs(world, registry, playTime, frameDt)` (a registry seeded with `registerBuiltinNodes` + `registerGameplayNodes`; `playTime` accumulates while playing). Stop restores via the M41 snapshot (the authored `logicGraph` string is part of the scene, untouched).
- **Demo** — seed one scene entity (a cube) with a bob/move logic graph so the visual gate has something immediately (authored via the editor also works).

## Data flow

author graph (editor) → "Assign to selected entity" → `SceneEntity::logicGraph` → **Play** builds `World` + `LogicGraph` component → each frame `tickLogicGraphs` runs it with `{world, self, time, dt}` → nodes read/write the entity's `Transform` → it moves on screen → **Stop** restores.

## Error handling

- Gameplay node with `domainContext == nullptr` or no `Transform` on `self` → no-op (safe editor preview; missing component tolerated).
- Empty/malformed `SceneEntity::logicGraph` → no `LogicGraph` added (build skips it); `fromJson` already fails safe.
- Graph with no entry node → `iron::run` warns + no-ops (M53 behavior).
- Persistent `vars` default to zero on first access (`RunContext.vars` lookup miss → `GetVar` returns 0).

## Testing

- **`tests/test_gameplay_nodes.cpp`** — World + entity + `Transform`; run small graphs with a `GameContext`: `OnTick→Translate` moves position by `dt*speed`; `GetPosition→BreakVec3→…→MakeVec3→SetPosition` sets it; `MakeVec3`/`BreakVec3`/`Mul`/`Sin` math; `SetVar` then `GetVar` round-trips through `RunContext.vars`; a gameplay node with `domainContext==nullptr` no-ops without crashing.
- **`tests/test_logic_runtime.cpp`** — world + entity + `Transform` + `LogicGraph` (move graph); `tickLogicGraphs` N times; assert the position advanced ~`N*dt*speed` and a `SetVar` counter persists across ticks.
- The editor-assign + Play visuals are **visual-gated** (a cube bobs/moves in Play, authored as nodes; Stop restores its start position).

## Files

**New:** `engine/gameplay/GameContext.h`, `engine/gameplay/GameplayNodes.{h,cpp}`, `engine/gameplay/LogicGraph.h`, `engine/gameplay/LogicRuntime.{h,cpp}`; `tests/test_gameplay_nodes.cpp`, `tests/test_logic_runtime.cpp`.
**Modified:** `engine/nodes/NodeContext.h` (+`domainContext`), `engine/nodes/NodeRegistry.h` (+`isEntry`), `engine/nodes/BuiltinNodes.cpp` (Entry `isEntry=true`), `engine/nodes/GraphEvaluator.cpp` (entry-by-`isEntry`), `engine/scene/SceneFormat.h` (+`logicGraph` field + reflection registration), `engine/editor/NodeGraphPanel.{h,cpp}` (+Assign intent), `games/11-sandbox/main.cpp` (build + Play tick + assign + demo), `engine/CMakeLists.txt`, `tests/CMakeLists.txt`.

## Out of scope (→ later)

Collision/input events (`OnHit`/`OnKey`), spawn/destroy-entity nodes, component access beyond `Transform`, an `OnStart` run-once event (the `started` flag is reserved for it), node-debugging/breakpoints, hot-reload of running graphs, multiple entry/event nodes per graph, sharing one graph asset across many entities (each entity holds its own copy for now).
