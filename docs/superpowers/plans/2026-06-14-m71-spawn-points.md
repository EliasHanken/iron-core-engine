# M71 Spawn Points + Runtime Spawning + World-Space Nodes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Logic graphs can spawn prefabs into the live game at runtime, at placed `SpawnPoint` markers, plus read/write entity positions in world space.

**Architecture:** Additive. A `SpawnPoint` marker component (standard reflected component). Two world-space transform nodes and three spawn nodes added to the existing gameplay node set. A `SpawnPrefab` node enqueues a `SpawnRequest` onto a queue carried on `GameContext`; the sandbox host drains it after the logic tick and runs the M70 instantiate path. Spawned entities are ephemeral (discarded by the existing Play→Stop snapshot). Spawn-point queries iterate the per-entity `ComponentSet` already mirrored into the World by `spawnRuntime` (M68).

**Tech Stack:** C++20, CMake + CTest, custom math/reflection/node systems, Dear ImGui (host). Runtime lib `ironcore` (component + nodes + context), host `games/11-sandbox`. Tests via `tests/test_framework.h` (`CHECK`, `CHECK_NEAR`, `iron_test_result()`), registered with `iron_add_test`.

**Build & test (Windows / PowerShell, via Bash tool / Git Bash):**
- `ironcore` + tests: `cmake --build build --config Debug`; one test: `ctest --test-dir build -C Debug --output-on-failure -R <name>`
- Sandbox (Vulkan, has the editor): `cmake --build build-vk --config Debug --target sandbox`
- A full `cmake --build build` shows PRE-EXISTING editor-test link errors in `build/` (the editor is Vulkan-only); ignore them — verify via the specific `-R <name>` test, which links only `ironcore`.

**Spec:** `docs/superpowers/specs/2026-06-14-m71-spawn-points-design.md`

**Branch:** Work continues on `m71-spawn-points` (stacked on `m70-prefabs`, which is in open PR #104).

---

## Confirmed APIs (read before implementing)
- Node registration (`engine/gameplay/GameplayNodes.cpp`): `r.registerType({ "Name", "Category", { PortDesc{...} }, [](NodeContext& c){...}, isPure /*bool*/, "tooltip" });` with `using P = PortDesc; PortDir::In/Out`. `PortType` has `Exec, Bool, Int, Float, Vec2, Vec3, Vec4, String`.
- `NodeValue`: `::V3(Vec3)`, `::B(bool)`, `::S(std::string)`, `::F(float)`; readers `.asVec3()`, `.asBool()`, `.asString()`, `.asFloat()`.
- Node self/world access: `GameContext* g = gameOf(c);` (file-local helper, already present). `g->world` (`World*`), `g->self` (`EntityId`).
- `worldMatrix(const World&, EntityId)` from `engine/world/WorldHierarchy.h` → `Mat4`; translation is `m.at(0,3), m.at(1,3), m.at(2,3)`.
- `inverse(const Mat4&)` and `Mat4 * Vec4` from `engine/math/Mat4.h`. `Vec4{x,y,z,w}` from `engine/math/Vec.h`.
- `Parent` from `engine/world/Parent.h`: `struct Parent { EntityId parent; }`; `parent.valid()`.
- `ComponentSet` (`engine/world/ComponentSet.h`): `template<class T> T* add(const T&)`, `template<class T> const T* get() const` / non-const `get<T>()`. Mirrored into the World per entity by `spawnRuntime` via `world.add<ComponentSet>(...)`.
- Component reflection template: `Health.h` + `Health.reflect.cpp` (`r.registerType<T>("Name").field("f", &T::f)`), declared in `engine/reflection/RegisterCoreTypes.h`, registered as a component in `engine/scene/RegisterCoreComponents.cpp`. `bool`/`std::string` fields are supported (`TypeId::Bool`/`TypeId::String` in `ReflectionIO.cpp`).
- `tickLogicGraphs(World&, const NodeRegistry&, float time, float dt)` in `engine/gameplay/LogicRuntime.{h,cpp}` builds the per-entity `GameContext` internally.

---

## File Structure

**Create:**
- `engine/gameplay/SpawnPoint.h` — `struct SpawnPoint { std::string group; bool enabled; }`.
- `engine/gameplay/SpawnPoint.reflect.cpp` — `registerSpawnPoint(Reflection&)`.
- `tests/test_world_space_nodes.cpp` — GetWorldPosition / SetWorldPosition tests.
- `tests/test_spawn_nodes.cpp` — spawn-query + SpawnPrefab + RNG tests.
- `tests/test_spawn_point_io.cpp` — SpawnPoint serialization round-trip.

**Modify:**
- `engine/reflection/RegisterCoreTypes.h` — declare `registerSpawnPoint`.
- `engine/scene/RegisterCoreComponents.cpp` — register `SpawnPoint` component.
- `engine/gameplay/GameContext.h` — `SpawnRequest`, two `GameContext` fields, `nextRandomU32`.
- `engine/gameplay/LogicRuntime.h` / `LogicRuntime.cpp` — thread `spawnQueue`/`rngState` into the per-entity context (signature extension, default-null).
- `engine/gameplay/GameplayNodes.cpp` — 5 new nodes.
- `engine/CMakeLists.txt` — add `gameplay/SpawnPoint.reflect.cpp`.
- `tests/CMakeLists.txt` — register the 3 new tests.
- `games/11-sandbox/main.cpp` — `registerSpawnPoint`, spawn queue + rng, thread into `tickLogicGraphs`, drain-and-instantiate after the tick.

**Task order:** 1 (component) → 2 (world-space nodes) → 3 (context + rng + runtime signature) → 4 (spawn nodes) → 5 (host wiring) → 6 (regression + demo gate).

---

## Task 1: `SpawnPoint` marker component

**Files:**
- Create: `engine/gameplay/SpawnPoint.h`, `engine/gameplay/SpawnPoint.reflect.cpp`
- Modify: `engine/reflection/RegisterCoreTypes.h`, `engine/scene/RegisterCoreComponents.cpp`, `engine/CMakeLists.txt`
- Test: `tests/test_spawn_point_io.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Register the test**

In `tests/CMakeLists.txt`, after `iron_add_test(test_prefab_io test_prefab_io.cpp)`, add:

```cmake
iron_add_test(test_spawn_point_io test_spawn_point_io.cpp)
```

- [ ] **Step 2: Create the component header**

Create `engine/gameplay/SpawnPoint.h`:

```cpp
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
```

- [ ] **Step 3: Write the failing serialization test**

Create `tests/test_spawn_point_io.cpp` (round-trips a SpawnPoint through scene IO; setup mirrors `test_prefab_io.cpp`):

```cpp
#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "gameplay/SpawnPoint.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/RegisterCoreComponents.h"
#include "world/ComponentRegistry.h"
#include "test_framework.h"

static iron::Reflection makeReflection() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    iron::registerCollisionShape(r);
    iron::registerAudioEmitter(r);
    iron::registerReflectionProbe(r);
    iron::registerLogicGraphComponent(r);
    iron::registerHealth(r);
    iron::registerSpawnPoint(r);   // M71
    return r;
}

static void test_spawn_point_roundtrip() {
    iron::Reflection r = makeReflection();
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);

    iron::SceneFile s;
    iron::SceneEntity e; e.name = "spawn";
    e.components.add<iron::SpawnPoint>(iron::SpawnPoint{"enemy", false});
    s.entities = {e};

    const std::string js = iron::sceneToJsonString(r, cr, s);
    auto loaded = iron::sceneFromJsonString(r, cr, js);
    CHECK(loaded.has_value());
    CHECK(loaded->entities.size() == 1);
    const iron::SpawnPoint* sp = loaded->entities[0].components.get<iron::SpawnPoint>();
    CHECK(sp != nullptr);
    CHECK(sp && sp->group == "enemy");
    CHECK(sp && sp->enabled == false);
}

int main() {
    test_spawn_point_roundtrip();
    return iron_test_result();
}
```

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: COMPILE ERROR — `registerSpawnPoint` undeclared / `SpawnPoint.reflect.cpp` not built.

- [ ] **Step 5: Declare + implement the reflection registration**

In `engine/reflection/RegisterCoreTypes.h`, after `void registerHealth(Reflection& r);`, add:

```cpp
void registerSpawnPoint(Reflection& r);
```

Create `engine/gameplay/SpawnPoint.reflect.cpp`:

```cpp
#include "gameplay/SpawnPoint.h"
#include "reflection/Reflection.h"

namespace iron {

void registerSpawnPoint(Reflection& r) {
    r.registerType<SpawnPoint>("SpawnPoint")
        .field("group",   &SpawnPoint::group)
        .field("enabled", &SpawnPoint::enabled);
}

}  // namespace iron
```

- [ ] **Step 6: Register as a component + add the source to CMake**

In `engine/scene/RegisterCoreComponents.cpp`, add the include with the others:

```cpp
#include "gameplay/SpawnPoint.h"
```

and inside `registerCoreComponents`, after the `Health` line:

```cpp
    cr.registerComponent<SpawnPoint>("SpawnPoint", r);   // M71 spawn marker
```

In `engine/CMakeLists.txt`, after `gameplay/Health.reflect.cpp`, add:

```cmake
  gameplay/SpawnPoint.reflect.cpp
```

- [ ] **Step 7: Run the test**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_spawn_point_io`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add engine/gameplay/SpawnPoint.h engine/gameplay/SpawnPoint.reflect.cpp engine/reflection/RegisterCoreTypes.h engine/scene/RegisterCoreComponents.cpp engine/CMakeLists.txt tests/test_spawn_point_io.cpp tests/CMakeLists.txt
git commit -m "M71: SpawnPoint marker component (reflected, serialized via ComponentSet)"
```
End with a blank line then `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Task 2: World-space transform nodes

**Files:**
- Modify: `engine/gameplay/GameplayNodes.cpp` (add includes + 2 nodes)
- Test: `tests/test_world_space_nodes.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Register the test**

In `tests/CMakeLists.txt`, after the `test_spawn_point_io` line, add:

```cmake
iron_add_test(test_world_space_nodes test_world_space_nodes.cpp)
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_world_space_nodes.cpp`:

```cpp
#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Parent.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

using namespace iron;

static NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}

// Parent at +5 on X, child local +2 -> child world X = 7.
static void test_get_world_position_through_parent() {
    World w;
    EntityId parent = w.create();
    Transform pt; pt.position = {5, 0, 0}; w.add<Transform>(parent, pt);
    EntityId child = w.create();
    Transform ct; ct.position = {2, 0, 0}; w.add<Transform>(child, ct);
    w.add<Parent>(child, Parent{parent});

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gwp  = g.addNode("GetWorldPosition");
    const NodeId out  = g.addNode("SetOutput");
    g.setLiteral(out, "key", NodeValue::S("wp"));
    g.connect(gwp, "pos", out, "value");
    g.connect(tick, "then", out, "in");

    GameContext gc{&w, child, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK_NEAR(ctx.outputs.at("wp").asVec3().x, 7.0f);
}

// Setting world X=7 on a child of a parent at +5 stores local X=2.
static void test_set_world_position_through_parent() {
    World w;
    EntityId parent = w.create();
    Transform pt; pt.position = {5, 0, 0}; w.add<Transform>(parent, pt);
    EntityId child = w.create();
    Transform ct; ct.position = {0, 0, 0}; w.add<Transform>(child, ct);
    w.add<Parent>(child, Parent{parent});

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId swp  = g.addNode("SetWorldPosition");
    g.setLiteral(swp, "pos", NodeValue::V3(Vec3{7, 0, 0}));
    g.connect(tick, "then", swp, "in");

    GameContext gc{&w, child, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK_NEAR(w.get<Transform>(child)->position.x, 2.0f);   // local
}

// On a root (no Parent), set world == local.
static void test_set_world_position_root() {
    World w;
    EntityId e = w.create(); w.add<Transform>(e, Transform{});
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId swp  = g.addNode("SetWorldPosition");
    g.setLiteral(swp, "pos", NodeValue::V3(Vec3{9, 0, 0}));
    g.connect(tick, "then", swp, "in");

    GameContext gc{&w, e, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK_NEAR(w.get<Transform>(e)->position.x, 9.0f);
}

int main() {
    test_get_world_position_through_parent();
    test_set_world_position_through_parent();
    test_set_world_position_root();
    return iron_test_result();
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: the new test builds but FAILS at runtime / or the evaluator reports unknown node types `GetWorldPosition`/`SetWorldPosition` (nodes not yet registered). Either way it does not pass.

- [ ] **Step 4: Add includes + implement the nodes**

In `engine/gameplay/GameplayNodes.cpp`, add to the includes at the top (after `#include "world/World.h"`):

```cpp
#include "math/Mat4.h"
#include "world/Parent.h"
#include "world/WorldHierarchy.h"
```

Then inside `registerGameplayNodes`, after the `Translate` node registration, add:

```cpp
    r.registerType({"GetWorldPosition", "Transform",
        { P{"pos", PortType::Vec3, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            Vec3 pos{0, 0, 0};
            if (g && g->world && g->world->get<Transform>(g->self)) {
                const Mat4 wm = worldMatrix(*g->world, g->self);
                pos = Vec3{wm.at(0, 3), wm.at(1, 3), wm.at(2, 3)};
            }
            c.out("pos", NodeValue::V3(pos));
        }, false, "Self position in world space"});

    r.registerType({"SetWorldPosition", "Transform",
        { P{"in", PortType::Exec, In}, P{"pos", PortType::Vec3, In},
          P{"then", PortType::Exec, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            if (g && g->world) {
                if (Transform* t = g->world->get<Transform>(g->self)) {
                    const Vec3 wp = c.in("pos").asVec3();
                    const Parent* p = g->world->get<Parent>(g->self);
                    if (p && p->parent.valid()) {
                        // local = inverse(parentWorld) * worldPoint
                        const Mat4 inv = inverse(worldMatrix(*g->world, p->parent));
                        const Vec4 local = inv * Vec4{wp.x, wp.y, wp.z, 1.0f};
                        t->position = Vec3{local.x, local.y, local.z};
                    } else {
                        t->position = wp;   // root: world == local
                    }
                }
            }
            c.fire("then");
        }, false, "Set self position in world space"});
```

- [ ] **Step 5: Run the test**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_world_space_nodes`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/gameplay/GameplayNodes.cpp tests/test_world_space_nodes.cpp tests/CMakeLists.txt
git commit -m "M71: GetWorldPosition / SetWorldPosition gameplay nodes (+ tests)"
```
End with the Co-Authored-By trailer.

---

## Task 3: `GameContext` spawn fields + RNG + runtime signature

**Files:**
- Modify: `engine/gameplay/GameContext.h`, `engine/gameplay/LogicRuntime.h`, `engine/gameplay/LogicRuntime.cpp`
- Test: extend `tests/test_gameplay_nodes.cpp` (RNG determinism)

This task is plumbing for Task 4/5: it adds the spawn-request queue + RNG to the per-node context and threads them through `tickLogicGraphs`. No node uses them yet; verified by the RNG check + existing gameplay tests still passing (proves `GameContext` aggregate-init back-compat).

- [ ] **Step 1: Extend `GameContext.h`**

Replace `engine/gameplay/GameContext.h` with:

```cpp
#pragma once

#include "math/Vec.h"
#include "world/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

class World;

// M71: a queued request to instantiate a prefab at a world position. The
// SpawnPrefab node pushes these; the host drains them after the logic tick and
// runs the M70 instantiate path.
struct SpawnRequest {
    std::string prefabPath;
    Vec3        position;
};

// The gameplay-domain context a logic graph runs against. Set into
// RunContext::domainContext (as a void*) by the runtime each tick.
struct GameContext {
    World*   world = nullptr;
    EntityId self = {};        // the entity owning the running graph
    float    time = 0.0f;      // elapsed Play seconds
    float    deltaTime = 0.0f;
    // M71: host-owned, nullable. Nodes use these without depending on the host
    // or the renderer; null when running headless / in the editor preview.
    std::vector<SpawnRequest>* spawnQueue = nullptr;
    std::uint32_t*             rngState   = nullptr;
};

// M71: xorshift32. `state` must be non-zero; advances it and returns the new
// value. Host owns the state (seeded at Play start) so a Play run is
// reproducible per seed and nodes stay pure.
inline std::uint32_t nextRandomU32(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace iron
```

(The two new fields have defaults, so existing aggregate initialisers like
`GameContext{&w, e, 0.0f, 0.1f}` remain valid.)

- [ ] **Step 2: Extend the runtime signature**

In `engine/gameplay/LogicRuntime.h`, replace the `tickLogicGraphs` declaration with one that accepts the optional queue + rng. First check the current header's includes; ensure it can name `SpawnRequest` and `std::uint32_t` by adding:

```cpp
#include "gameplay/GameContext.h"

#include <cstdint>
#include <vector>
```

and the declaration:

```cpp
// Tick every entity's LogicGraph. `spawnQueue` / `rngState` (M71) are forwarded
// into each entity's GameContext; pass nullptr (the defaults) when spawning /
// randomness are not needed (headless tests, non-Play ticks).
void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime,
                     std::vector<SpawnRequest>* spawnQueue = nullptr,
                     std::uint32_t* rngState = nullptr);
```

- [ ] **Step 3: Thread them through `LogicRuntime.cpp`**

In `engine/gameplay/LogicRuntime.cpp`, update the definition signature and the `GameContext` construction:

```cpp
void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime,
                     std::vector<SpawnRequest>* spawnQueue,
                     std::uint32_t* rngState) {
    world.view<LogicGraph>().forEach([&](EntityId e, LogicGraph& lg) {
        GameContext gc{&world, e, time, deltaTime, spawnQueue, rngState};
        RunContext ctx;
        ctx.vars = std::move(lg.vars);
        ctx.domainContext = &gc;
        run(lg.graph, registry, ctx);
        lg.vars = std::move(ctx.vars);
    });
}
```

- [ ] **Step 4: Add an RNG determinism check to the existing gameplay test**

In `tests/test_gameplay_nodes.cpp`, add a block inside `main()` (before `return iron_test_result();`):

```cpp
    // M71: nextRandomU32 is deterministic per seed and advances.
    {
        std::uint32_t a = 2463534242u, b = 2463534242u;
        const std::uint32_t a1 = nextRandomU32(a);
        const std::uint32_t b1 = nextRandomU32(b);
        CHECK(a1 == b1);            // same seed -> same value
        CHECK(a1 != 2463534242u);   // it actually changed
        CHECK(nextRandomU32(a) == nextRandomU32(b));  // sequences stay in lockstep
    }
```

(`GameContext.h` is already included by this test, so `nextRandomU32` is in scope. Add `#include <cstdint>` to the test if the compiler complains.)

- [ ] **Step 5: Build + run gameplay tests**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R "test_gameplay_nodes|test_logic_runtime"`
Expected: PASS (proves the signature change + GameContext extension didn't break existing logic-graph behavior).

- [ ] **Step 6: Commit**

```bash
git add engine/gameplay/GameContext.h engine/gameplay/LogicRuntime.h engine/gameplay/LogicRuntime.cpp tests/test_gameplay_nodes.cpp
git commit -m "M71: GameContext spawn queue + rng; tickLogicGraphs forwards them"
```
End with the Co-Authored-By trailer.

---

## Task 4: Spawn nodes — `GetSpawnPoint`, `GetRandomSpawnPoint`, `SpawnPrefab`

**Files:**
- Modify: `engine/gameplay/GameplayNodes.cpp` (includes + 3 nodes)
- Test: `tests/test_spawn_nodes.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Register the test**

In `tests/CMakeLists.txt`, after the `test_world_space_nodes` line, add:

```cmake
iron_add_test(test_spawn_nodes test_spawn_nodes.cpp)
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_spawn_nodes.cpp`:

```cpp
#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "gameplay/SpawnPoint.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/ComponentSet.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

#include <cstdint>

using namespace iron;

static NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}

// A spawn point as the runtime sees it: a Transform + a ComponentSet carrying a
// SpawnPoint (matching how spawnRuntime mirrors authored components).
static EntityId makeMarker(World& w, Vec3 pos, std::string group, bool enabled) {
    EntityId e = w.create();
    Transform t; t.position = pos; w.add<Transform>(e, t);
    ComponentSet cs; cs.add<SpawnPoint>(SpawnPoint{std::move(group), enabled});
    w.add<ComponentSet>(e, cs);
    return e;
}

static void test_get_spawn_point_first_enabled() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", true);
    makeMarker(w, {2, 0, 0}, "enemy", true);
    makeMarker(w, {9, 9, 9}, "player", true);   // wrong group

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gsp  = g.addNode("GetSpawnPoint");
    const NodeId seq  = g.addNode("Sequence");
    const NodeId oPos = g.addNode("SetOutput");
    const NodeId oFnd = g.addNode("SetOutput");
    g.setLiteral(gsp, "group", NodeValue::S("enemy"));
    g.setLiteral(oPos, "key", NodeValue::S("pos"));
    g.setLiteral(oFnd, "key", NodeValue::S("found"));
    g.connect(gsp, "pos",   oPos, "value");
    g.connect(gsp, "found", oFnd, "value");
    g.connect(tick, "then", seq, "in");
    g.connect(seq, "0", oPos, "in");
    g.connect(seq, "1", oFnd, "in");

    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(ctx.outputs.at("found").asBool() == true);
    CHECK_NEAR(ctx.outputs.at("pos").asVec3().x, 1.0f);   // first enabled enemy
}

static void test_get_spawn_point_none_found() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", false);   // disabled
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gsp  = g.addNode("GetSpawnPoint");
    const NodeId oFnd = g.addNode("SetOutput");
    g.setLiteral(gsp, "group", NodeValue::S("enemy"));
    g.setLiteral(oFnd, "key", NodeValue::S("found"));
    g.connect(gsp, "found", oFnd, "value");
    g.connect(tick, "then", oFnd, "in");

    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(ctx.outputs.at("found").asBool() == false);
}

static void test_get_random_spawn_point_member_of_group() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", true);
    makeMarker(w, {2, 0, 0}, "enemy", true);
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId grp  = g.addNode("GetRandomSpawnPoint");
    const NodeId oPos = g.addNode("SetOutput");
    g.setLiteral(grp, "group", NodeValue::S("enemy"));
    g.setLiteral(oPos, "key", NodeValue::S("pos"));
    g.connect(grp, "pos", oPos, "value");
    g.connect(tick, "then", oPos, "in");

    std::uint32_t seed = 12345u;
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    gc.rngState = &seed;
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    const float x = ctx.outputs.at("pos").asVec3().x;
    CHECK(x == 1.0f || x == 2.0f);   // one of the group's markers
}

static void test_spawn_prefab_enqueues() {
    World w;
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId sp   = g.addNode("SpawnPrefab");
    g.setLiteral(sp, "prefab", NodeValue::S("enemy.prefab"));
    g.setLiteral(sp, "pos",    NodeValue::V3(Vec3{1, 2, 3}));
    g.connect(tick, "then", sp, "in");

    std::vector<SpawnRequest> queue;
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    gc.spawnQueue = &queue;
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(queue.size() == 1);
    CHECK(queue.size() == 1 && queue[0].prefabPath == "enemy.prefab");
    CHECK(queue.size() == 1 && std::fabs(queue[0].position.x - 1.0f) < 1e-4f);
    CHECK(queue.size() == 1 && std::fabs(queue[0].position.z - 3.0f) < 1e-4f);
}

static void test_spawn_prefab_null_queue_is_safe() {
    World w;
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId sp   = g.addNode("SpawnPrefab");
    g.setLiteral(sp, "prefab", NodeValue::S("x.prefab"));
    g.connect(tick, "then", sp, "in");
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};   // spawnQueue == nullptr
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);                              // must not crash
    CHECK(true);
}

int main() {
    test_get_spawn_point_first_enabled();
    test_get_spawn_point_none_found();
    test_get_random_spawn_point_member_of_group();
    test_spawn_prefab_enqueues();
    test_spawn_prefab_null_queue_is_safe();
    return iron_test_result();
}
```

(Add `#include <cmath>` to the test if `std::fabs` is unresolved.)

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: the test builds but the spawn nodes are unknown to the evaluator → assertions fail / `found` key missing. Not passing.

- [ ] **Step 4: Add includes + implement the three nodes**

In `engine/gameplay/GameplayNodes.cpp`, add includes (with the others added in Task 2):

```cpp
#include "gameplay/SpawnPoint.h"
#include "world/ComponentSet.h"

#include <string>
#include <vector>
```

Inside `registerGameplayNodes`, after the `SetWorldPosition` node, add:

```cpp
    r.registerType({"GetSpawnPoint", "Spawn",
        { P{"group", PortType::String, In},
          P{"pos", PortType::Vec3, Out}, P{"found", PortType::Bool, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            Vec3 pos{0, 0, 0};
            bool found = false;
            if (g && g->world) {
                const std::string group = c.in("group").asString();
                g->world->view<ComponentSet>().forEach([&](EntityId e, ComponentSet& cs) {
                    if (found) return;
                    const SpawnPoint* sp = cs.get<SpawnPoint>();
                    if (!sp || !sp->enabled || sp->group != group) return;
                    const Mat4 wm = worldMatrix(*g->world, e);
                    pos = Vec3{wm.at(0, 3), wm.at(1, 3), wm.at(2, 3)};
                    found = true;
                });
            }
            c.out("pos", NodeValue::V3(pos));
            c.out("found", NodeValue::B(found));
        }, false, "First enabled spawn point in group (world position)"});

    r.registerType({"GetRandomSpawnPoint", "Spawn",
        { P{"group", PortType::String, In},
          P{"pos", PortType::Vec3, Out}, P{"found", PortType::Bool, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            Vec3 pos{0, 0, 0};
            bool found = false;
            if (g && g->world) {
                const std::string group = c.in("group").asString();
                std::vector<Vec3> matches;
                g->world->view<ComponentSet>().forEach([&](EntityId e, ComponentSet& cs) {
                    const SpawnPoint* sp = cs.get<SpawnPoint>();
                    if (!sp || !sp->enabled || sp->group != group) return;
                    const Mat4 wm = worldMatrix(*g->world, e);
                    matches.push_back(Vec3{wm.at(0, 3), wm.at(1, 3), wm.at(2, 3)});
                });
                if (!matches.empty()) {
                    std::size_t idx = 0;
                    if (g->rngState)
                        idx = nextRandomU32(*g->rngState) % matches.size();
                    pos = matches[idx];
                    found = true;
                }
            }
            c.out("pos", NodeValue::V3(pos));
            c.out("found", NodeValue::B(found));
        }, false, "Random enabled spawn point in group (world position)"});

    r.registerType({"SpawnPrefab", "Spawn",
        { P{"in", PortType::Exec, In}, P{"prefab", PortType::String, In},
          P{"pos", PortType::Vec3, In}, P{"then", PortType::Exec, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            if (g && g->spawnQueue)
                g->spawnQueue->push_back(
                    SpawnRequest{c.in("prefab").asString(), c.in("pos").asVec3()});
            c.fire("then");
        }, false, "Request a prefab spawn at a world position"});
```

- [ ] **Step 5: Run the test**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_spawn_nodes`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/gameplay/GameplayNodes.cpp tests/test_spawn_nodes.cpp tests/CMakeLists.txt
git commit -m "M71: GetSpawnPoint / GetRandomSpawnPoint / SpawnPrefab nodes (+ tests)"
```
End with the Co-Authored-By trailer.

---

## Task 5: Host wiring — seed RNG, thread the queue, drain after the tick

**Files:**
- Modify: `games/11-sandbox/main.cpp`

Anchors (confirm by reading before editing — line numbers approximate):
- `iron::registerHealth(reflection);` (~187)
- `float playTime = 0.0f; float playDt = 0.0f;` (~677)
- `spawnRuntime` lambda (~706) / `togglePlayMode` Edit→Play branch (~823-831)
- `iron::tickLogicGraphs(world, nodeRegistry, playTime, playDt);` (~1961)
- The prefab-instantiate World-spawn loop in the `PbAction::Instantiate` branch (added in M70) — copy its body for the drain.

- [ ] **Step 1: Register SpawnPoint reflection in the host**

After `iron::registerHealth(reflection);`, add:

```cpp
    iron::registerSpawnPoint(reflection);   // M71
```

(`registerSpawnPoint` is declared in `reflection/RegisterCoreTypes.h`, already included by main.cpp via the other `register*` calls — confirm the include is present; it is what declares `registerHealth`.)

- [ ] **Step 2: Declare the spawn queue + RNG state**

Near `float playTime = 0.0f;` (~677), add:

```cpp
    std::vector<iron::SpawnRequest> spawnQueue;   // M71: drained each Play frame
    std::uint32_t                   spawnRng = 0; // M71: seeded at Play start
    std::unordered_map<std::string, std::optional<iron::Prefab>> spawnPrefabCache; // M71
```

Ensure `#include "gameplay/GameContext.h"` (for `SpawnRequest`), `<optional>`, `<unordered_map>`, and `<cstdint>` are present among the includes (most already are).

- [ ] **Step 3: Seed the RNG at Play start**

In `togglePlayMode`, in the Edit→Play branch next to `playTime = 0.0f;`, add:

```cpp
            spawnRng  = 0x9E3779B9u;   // M71: fixed seed -> reproducible Play runs
            spawnQueue.clear();
```

(Any non-zero constant works for xorshift; a fixed seed keeps Play runs reproducible.)

- [ ] **Step 4: Thread the queue + rng into the tick**

Replace the tick call (~1961):

```cpp
            iron::tickLogicGraphs(world, nodeRegistry, playTime, playDt);
```

with:

```cpp
            iron::tickLogicGraphs(world, nodeRegistry, playTime, playDt,
                                  &spawnQueue, &spawnRng);   // M71
```

- [ ] **Step 5: Drain the queue right after the tick**

Immediately after that `tickLogicGraphs(...)` call, add the drain (reuses the M70 instantiate World-spawn pattern; cap per frame to bound a runaway graph):

```cpp
            // M71: drain spawn requests -> instantiate prefabs into the live game.
            // Ephemeral: the Play->Stop snapshot discards them. Capped per frame.
            constexpr int kMaxSpawnsPerFrame = 64;
            int spawnsThisFrame = 0;
            for (const iron::SpawnRequest& req : spawnQueue) {
                if (spawnsThisFrame >= kMaxSpawnsPerFrame) {
                    iron::Log::warn("sandbox: spawn cap (%d/frame) hit; %zu dropped",
                                    kMaxSpawnsPerFrame, spawnQueue.size() - spawnsThisFrame);
                    break;
                }
                // Cache prefab loads (success or failure) by path.
                auto it = spawnPrefabCache.find(req.prefabPath);
                if (it == spawnPrefabCache.end()) {
                    it = spawnPrefabCache.emplace(
                        req.prefabPath,
                        iron::loadPrefabFile(reflection, componentRegistry, req.prefabPath)).first;
                    if (!it->second)
                        iron::Log::error("sandbox: spawn failed to load prefab %s",
                                         req.prefabPath.c_str());
                }
                if (!it->second || it->second->entities.empty()) continue;
                const iron::Prefab& pf = *it->second;

                iron::Transform placement = pf.entities[0].transform;
                placement.position = req.position;
                iron::instantiatePrefab(scene, pf, placement, uniqueName);

                for (int i = static_cast<int>(sceneIndexToEntity.size());
                     i < static_cast<int>(scene.entities.size()); ++i) {
                    const iron::EntityId entity = world.create();
                    ResolvedEntity re;
                    if (resolveEntity(scene.entities[i], i, re)) {
                        resolved.push_back(re);
                        world.add<iron::RenderHandles>(entity, toRenderHandles(re));
                    } else {
                        iron::Log::warn("sandbox: spawned entity '%s' failed to resolve",
                                        scene.entities[i].name.c_str());
                    }
                    const iron::SceneEntity& se = scene.entities[i];
                    world.add<iron::Transform>(entity, se.transform);
                    world.add<iron::MeshRef>(entity, se.mesh);
                    world.add<iron::MaterialDef>(entity, se.material);
                    world.add<iron::ComponentSet>(entity, se.components);  // M71: visible to nodes
                    sceneIndexToEntity.push_back(entity);
                }
                mirrorParents();
                ++spawnsThisFrame;
            }
            spawnQueue.clear();
```

> Confirm `uniqueName`, `resolveEntity`, `toRenderHandles`, `ResolvedEntity`, `mirrorParents`, `resolved`, `sceneIndexToEntity`, `instantiatePrefab`, `loadPrefabFile`, `reflection`, `componentRegistry` are all in scope at the tick site (they are — the tick runs in the same `setRender` Play block, and these are used by the M70 Instantiate branch which lives in the same function). If the per-entity World-spawn body differs from the M70 Instantiate branch, MIRROR THE M70 BRANCH exactly (search `PbAction::Instantiate`) so spawned and instantiated entities get identical components — but add the `world.add<iron::ComponentSet>` line, which the M70 edit-time branch does not need (edit-time entities get their ComponentSet only at Play start via spawnRuntime).

- [ ] **Step 6: Build the sandbox**

Run: `cmake --build build-vk --config Debug --target sandbox`
Expected: clean build. Do not run it (GUI). If a symbol is out of scope, adjust per the note above and report.

- [ ] **Step 7: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M71: sandbox seeds rng, threads spawn queue into tick, drains to instantiate"
```
End with the Co-Authored-By trailer.

---

## Task 6: Full regression + demo gate

**Files:** none (verification only).

- [ ] **Step 1: Full build + all tests (Vulkan build has the editor + all tests)**

Run: `cmake --build build-vk --config Debug`
Then: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all tests pass — the prior suite plus `test_spawn_point_io`, `test_world_space_nodes`, `test_spawn_nodes`, with `test_gameplay_nodes` / `test_logic_runtime` still green (proves the `GameContext`/runtime changes were back-compatible). If a pre-existing test broke, STOP and use superpowers:systematic-debugging.

- [ ] **Step 2: User-run demo gate (spec acceptance)**

Ask the user to launch the sandbox (`build-vk/games/11-sandbox/Debug/sandbox.exe`) and confirm:
1. Add two entities, give each a `SpawnPoint` component (group `"enemy"`) in the Inspector, and place them apart. Save a simple prefab (e.g. a cube) as the "enemy" via the M70 Prefabs panel.
2. On a controller entity, author a logic graph: `OnTick` → (a timer via `SetVar`/`GetVar` or just every tick for the demo) → `GetRandomSpawnPoint("enemy")` → `SpawnPrefab(<enemyPrefabPath>, pos)`.
3. Press Play (F5): prefab instances appear at the spawn-point markers; with `GetRandomSpawnPoint` they alternate between the two markers.
4. A `SetWorldPosition` / `GetWorldPosition` node on a parented entity moves it correctly in world space.
5. Press Stop: all spawned entities are gone, the scene shows only the original entities; reload confirms the scene file is unchanged (only the two markers + controller).

- [ ] **Step 3: Branch completion**

M71 is stacked on `m70-prefabs` (PR #104, not yet merged). Once the demo gate passes, use superpowers:finishing-a-development-branch. If PR #104 has merged to `main` by then, rebase `m71-spawn-points` onto `main` before opening its PR; otherwise open the M71 PR with base `m70-prefabs` (a stacked PR) and note the dependency.

---

## Self-Review (completed during planning)

**Spec coverage:**
- World-space nodes (Get/SetWorldPosition, world→local via inverse(parentWorld)) → Task 2. ✓
- `SpawnPoint` component (group + enabled, reflected/serialized/Inspector) → Task 1. ✓
- Spawn-point query nodes (first + random, world position, found flag) → Task 4. ✓
- `SpawnPrefab` enqueues onto a `GameContext` queue; engine nodes do no IO/render → Tasks 3 (context), 4 (node). ✓
- Host drains the queue after the tick via the M70 instantiate path; per-frame cap; prefab-load cache → Task 5. ✓
- Ephemeral lifecycle via existing Play→Stop snapshot → Task 5 (no new teardown) + demo gate Task 6. ✓
- RNG host-seeded, reproducible, nodes pure → Tasks 3 (`nextRandomU32` + `rngState`), 5 (seed). ✓
- Spawn points visible via the `ComponentSet` mirror (not `view<SpawnPoint>`) → Task 4 implementation + spec §4 correction. ✓
- Error handling: null queue safe, bad prefab path skipped + logged, no matches → found=false, no-Transform no-op, spawn cap → Tasks 4, 5. ✓
- Testing: world-space round trips, spawn queries, enqueue, RNG, IO round-trip → Tasks 1–4. ✓

**Placeholder scan:** No TBD/TODO/"similar to". Every code step is complete. Two `>` notes flag confirmations against the real `main.cpp` scope/branch — confirmations, not placeholders.

**Type consistency:** `SpawnPoint{std::string group; bool enabled;}` identical across header/reflect/IO/test. `SpawnRequest{std::string prefabPath; Vec3 position;}` identical across `GameContext.h`, the `SpawnPrefab` node, and the host drain. `tickLogicGraphs(World&, const NodeRegistry&, float, float, std::vector<SpawnRequest>*, std::uint32_t*)` consistent between `LogicRuntime.h/.cpp` and the host call. Node names (`GetWorldPosition`, `SetWorldPosition`, `GetSpawnPoint`, `GetRandomSpawnPoint`, `SpawnPrefab`) and their port names (`pos`, `found`, `group`, `prefab`, `in`, `then`) are consistent between registration and tests. `nextRandomU32(std::uint32_t&)` consistent across definition, node, host. Host reuses `uniqueName`/`resolveEntity`/`toRenderHandles`/`ResolvedEntity`/`mirrorParents`/`resolved`/`sceneIndexToEntity` exactly as the M70 Instantiate branch.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-14-m71-spawn-points.md`.
