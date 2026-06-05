# M55 — Gameplay Logic Nodes + Runtime — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make node graphs drive entity behavior in Play — an `OnTick` event + world-aware nodes that read/write the owning entity's `Transform`, with persistent per-entity variables, authored in the M54 editor and run by a per-frame runtime.

**Architecture:** M53's core stays domain-agnostic; the only core change is an opaque `void* domainContext` on `RunContext`. A gameplay module defines `GameContext{World*,EntityId,time,dt}`, a world-aware node library, a `LogicGraph` component, and `tickLogicGraphs`. The sandbox stores a graph on `SceneEntity`, builds a `LogicGraph` on Play, and ticks it each frame.

**Tech Stack:** C++17, M53 `engine/nodes/`, `iron::World`/`Transform`/`EntityId` (`engine/world/`), the `test_framework.h` harness, ImGui/imgui-node-editor (editor assign). Canonical build dir `build-vk`; ctest `-C Debug`.

**Spec:** `docs/superpowers/specs/2026-06-05-m55-gameplay-logic-nodes-design.md`

**Conventions:** end every commit body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push (PR at end). Branch `m55-gameplay-logic-nodes` (off merged `main`).

**Risk note:** Tasks 1–4 are headless TDD (full code below). Task 5 (host integration: scene→World build, Play tick, editor "Assign", demo) is visual-gated — concrete guidance + the panel/host patterns to follow.

---

## File Structure

**New:** `engine/gameplay/GameContext.h`, `engine/gameplay/GameplayNodes.{h,cpp}`, `engine/gameplay/LogicGraph.h`, `engine/gameplay/LogicRuntime.{h,cpp}`; `tests/test_gameplay_nodes.cpp`, `tests/test_logic_runtime.cpp`.
**Modified:** `engine/nodes/NodeContext.h`, `engine/nodes/NodeRegistry.h`, `engine/nodes/BuiltinNodes.cpp`, `engine/nodes/GraphEvaluator.cpp`, `engine/world/ComponentArray.h`, `engine/scene/SceneFormat.h`, `engine/scene/SceneIO.cpp`, `engine/editor/NodeGraphPanel.{h,cpp}`, `games/11-sandbox/main.cpp`, `engine/CMakeLists.txt`, `tests/CMakeLists.txt`.

---

## Task 1: Core — `domainContext` + generalized entry (`isEntry`)

**Files:** Modify `engine/nodes/NodeContext.h`, `engine/nodes/NodeRegistry.h`, `engine/nodes/BuiltinNodes.cpp`, `engine/nodes/GraphEvaluator.cpp`; test `tests/test_node_graph.cpp`.

- [ ] **Step 1: Add `domainContext` to `RunContext` in `engine/nodes/NodeContext.h`**

In `struct RunContext`, after `int maxSteps = 1000;` add:
```cpp
    // Opaque per-domain context (gameplay sets a GameContext*, shaders/VFX
    // their own). nullptr when running headless / in the editor preview.
    void* domainContext = nullptr;
```

- [ ] **Step 2: Add `isEntry` to `NodeTypeDesc` in `engine/nodes/NodeRegistry.h`**

In `struct NodeTypeDesc`, add as the LAST member (so existing aggregate inits keep working):
```cpp
    bool isEntry = false;   // an entry/event node the evaluator starts from
```

- [ ] **Step 3: Mark `Entry` as an entry node in `engine/nodes/BuiltinNodes.cpp`**

Change the `Entry` registration to set `isEntry`:
```cpp
    r.registerType({"Entry", "Flow",
        { P{"then", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire("then"); }, true});
```
(The trailing `, true` is the `isEntry` arg.)

- [ ] **Step 4: Generalize entry detection in `engine/nodes/GraphEvaluator.cpp`**

In `run(...)`, replace the entry-finding loop:
```cpp
    NodeId entry = 0;
    for (const Node& n : graph.nodes()) {
        if (n.typeName == "Entry") { entry = n.id; break; }
    }
```
with:
```cpp
    NodeId entry = 0;
    for (const Node& n : graph.nodes()) {
        const NodeTypeDesc* t = registry.find(n.typeName);
        if (t && t->isEntry) { entry = n.id; break; }
    }
```

- [ ] **Step 5: Add a test to `tests/test_node_graph.cpp`** (before `return iron_test_result();`)

```cpp
    // A custom isEntry node (not named "Entry") drives the run.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        reg.registerType({"MyStart", "Flow",
            { PortDesc{"then", PortType::Exec, PortDir::Out} },
            [](NodeContext& c) { c.fire("then"); }, true});
        Graph g;
        const NodeId s = g.addNode("MyStart");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(set, "key", NodeValue::S("k"));
        g.setLiteral(set, "value", NodeValue::F(3.0f));
        g.connect(s, "then", set, "in");
        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("k").asFloat(), 3.0f);
    }
```
(The test file already includes the node headers + has `using namespace iron;`.)

- [ ] **Step 6: Build + run (existing Entry programs + the new isEntry test)**

Run: `cmake --build build-vk --config Debug --target test_node_graph && ctest --test-dir build-vk -C Debug -R test_node_graph --output-on-failure`
Expected: PASS (all existing Entry-based tests still pass — Entry now has `isEntry=true` — plus the new one).

- [ ] **Step 7: Commit**

```bash
git add engine/nodes/NodeContext.h engine/nodes/NodeRegistry.h engine/nodes/BuiltinNodes.cpp engine/nodes/GraphEvaluator.cpp tests/test_node_graph.cpp
git commit -m "M55: RunContext.domainContext + generalized entry nodes (isEntry)"
```

---

## Task 2: Gameplay node library (GameContext + world-aware nodes)

**Files:** Create `engine/gameplay/GameContext.h`, `engine/gameplay/GameplayNodes.h/.cpp`; modify `engine/CMakeLists.txt`, `tests/CMakeLists.txt`; test `tests/test_gameplay_nodes.cpp`.

- [ ] **Step 1: Create `engine/gameplay/GameContext.h`**

```cpp
#pragma once

#include "world/Entity.h"

namespace iron {

class World;

// The gameplay-domain context a logic graph runs against. Set into
// RunContext::domainContext (as a void*) by the runtime each tick.
struct GameContext {
    World*   world = nullptr;
    EntityId self;             // the entity owning the running graph
    float    time = 0.0f;      // elapsed Play seconds
    float    deltaTime = 0.0f;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/gameplay/GameplayNodes.h`**

```cpp
#pragma once

namespace iron {
class NodeRegistry;
// Register the world-aware node set: OnTick, GetPosition, SetPosition,
// Translate, MakeVec3, BreakVec3, Mul, Sin, GetVar, SetVar.
void registerGameplayNodes(NodeRegistry& registry);
}  // namespace iron
```

- [ ] **Step 3: Write the failing test `tests/test_gameplay_nodes.cpp`**

```cpp
#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

namespace {
NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}
}  // namespace

int main() {
    // OnTick -> Translate moves the self entity's position by the delta.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId mk   = g.addNode("MakeVec3");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(mk, "x", NodeValue::F(2.0f));
        g.connect(mk, "v", tr, "delta");
        g.connect(tick, "then", tr, "in");

        GameContext gc{&w, e, 0.0f, 0.1f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(w.get<Transform>(e)->position.x, 2.0f);
    }

    // GetPosition/BreakVec3/Mul/Sin path: set Y to sin(time)*amp via SetPosition.
    {
        World w; EntityId e = w.create();
        Transform t0; t0.position = Vec3{1.0f, 0.0f, 3.0f};
        w.add<Transform>(e, t0);
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getp = g.addNode("GetPosition");
        const NodeId br   = g.addNode("BreakVec3");
        const NodeId sinN = g.addNode("Sin");
        const NodeId mk   = g.addNode("MakeVec3");
        const NodeId setp = g.addNode("SetPosition");
        g.connect(getp, "pos", br, "v");
        // y = sin(time); time literal-defaulted to 0 -> sin(0)=0; keep x,z from break.
        g.connect(tick, "time", sinN, "x");
        g.connect(br, "x", mk, "x");
        g.connect(sinN, "result", mk, "y");
        g.connect(br, "z", mk, "z");
        g.connect(mk, "v", setp, "pos");
        g.connect(tick, "then", setp, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        const Vec3 p = w.get<Transform>(e)->position;
        CHECK_NEAR(p.x, 1.0f);            // preserved
        CHECK_NEAR(p.y, 0.0f);            // sin(0)
        CHECK_NEAR(p.z, 3.0f);            // preserved
    }

    // SetVar then GetVar round-trips through the persistent vars blackboard.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId setv = g.addNode("SetVar");
        const NodeId getv = g.addNode("GetVar");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(setv, "name", NodeValue::S("hp"));
        g.setLiteral(setv, "value", NodeValue::F(42.0f));
        g.setLiteral(getv, "name", NodeValue::S("hp"));
        g.setLiteral(out, "key", NodeValue::S("r"));
        g.connect(getv, "value", out, "value");
        g.connect(tick, "then", setv, "in");
        // SetVar has no exec out; reach SetOutput via a Sequence to run both.
        const NodeId seq = g.addNode("Sequence");
        g.disconnect(setv, "in");            // (rewire through Sequence)
        g.connect(tick, "then", seq, "in");
        g.connect(seq, "0", setv, "in");
        g.connect(seq, "1", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("r").asFloat(), 42.0f);
    }

    // No domainContext (editor preview): gameplay nodes no-op, no crash.
    {
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(tr, "delta", NodeValue::V3(Vec3{1, 1, 1}));
        g.connect(tick, "then", tr, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);                    // must not crash
        CHECK(true);
    }

    return iron_test_result();
}
```

- [ ] **Step 4: Register source + test, build to confirm RED**

In `engine/CMakeLists.txt`, after the `nodes/*.cpp` block, add:
```cmake
  gameplay/GameplayNodes.cpp
```
In `tests/CMakeLists.txt`, after `iron_add_test(test_graph_editor test_graph_editor.cpp)`:
```cmake
iron_add_test(test_gameplay_nodes test_gameplay_nodes.cpp)
```
Run: `cmake --build build-vk --config Debug --target test_gameplay_nodes`
Expected: FAIL — `registerGameplayNodes` undefined.

- [ ] **Step 5: Implement `engine/gameplay/GameplayNodes.cpp`**

```cpp
#include "gameplay/GameplayNodes.h"

#include "gameplay/GameContext.h"
#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"

#include <cmath>

namespace iron {

namespace {
GameContext* gameOf(NodeContext& c) {
    return static_cast<GameContext*>(c.run().domainContext);
}
Transform* selfTransform(NodeContext& c) {
    GameContext* g = gameOf(c);
    if (!g || !g->world) return nullptr;
    return g->world->get<Transform>(g->self);
}
}  // namespace

void registerGameplayNodes(NodeRegistry& r) {
    using P = PortDesc;
    const auto In  = PortDir::In;
    const auto Out = PortDir::Out;

    // OnTick: entry event; exposes dt + elapsed time.
    r.registerType({"OnTick", "Event",
        { P{"then", PortType::Exec, Out}, P{"dt", PortType::Float, Out},
          P{"time", PortType::Float, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            c.out("dt",   NodeValue::F(g ? g->deltaTime : 0.0f));
            c.out("time", NodeValue::F(g ? g->time : 0.0f));
            c.fire("then");
        }, true});

    r.registerType({"GetPosition", "Transform",
        { P{"pos", PortType::Vec3, Out} },
        [](NodeContext& c) {
            Transform* t = selfTransform(c);
            c.out("pos", NodeValue::V3(t ? t->position : Vec3{0, 0, 0}));
        }});

    r.registerType({"SetPosition", "Transform",
        { P{"in", PortType::Exec, In}, P{"pos", PortType::Vec3, In} },
        [](NodeContext& c) {
            if (Transform* t = selfTransform(c)) t->position = c.in("pos").asVec3();
        }});

    r.registerType({"Translate", "Transform",
        { P{"in", PortType::Exec, In}, P{"delta", PortType::Vec3, In} },
        [](NodeContext& c) {
            if (Transform* t = selfTransform(c))
                t->position = t->position + c.in("delta").asVec3();
        }});

    r.registerType({"MakeVec3", "Math",
        { P{"x", PortType::Float, In}, P{"y", PortType::Float, In},
          P{"z", PortType::Float, In}, P{"v", PortType::Vec3, Out} },
        [](NodeContext& c) {
            c.out("v", NodeValue::V3(Vec3{c.in("x").asFloat(), c.in("y").asFloat(),
                                          c.in("z").asFloat()}));
        }});

    r.registerType({"BreakVec3", "Math",
        { P{"v", PortType::Vec3, In}, P{"x", PortType::Float, Out},
          P{"y", PortType::Float, Out}, P{"z", PortType::Float, Out} },
        [](NodeContext& c) {
            const Vec3 v = c.in("v").asVec3();
            c.out("x", NodeValue::F(v.x));
            c.out("y", NodeValue::F(v.y));
            c.out("z", NodeValue::F(v.z));
        }});

    r.registerType({"Mul", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(c.in("a").asFloat() * c.in("b").asFloat()));
        }});

    r.registerType({"Sin", "Math",
        { P{"x", PortType::Float, In}, P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(std::sin(c.in("x").asFloat())));
        }});

    r.registerType({"GetVar", "Variable",
        { P{"name", PortType::String, In}, P{"value", PortType::Float, Out} },
        [](NodeContext& c) {
            auto& vars = c.run().vars;
            auto it = vars.find(c.in("name").asString());
            c.out("value", it != vars.end() ? it->second : NodeValue::F(0.0f));
        }});

    r.registerType({"SetVar", "Variable",
        { P{"in", PortType::Exec, In}, P{"name", PortType::String, In},
          P{"value", PortType::Float, In} },
        [](NodeContext& c) {
            c.run().vars[c.in("name").asString()] = c.in("value");
        }});
}

}  // namespace iron
```

- [ ] **Step 6: Build + run**

Run: `cmake --build build-vk --config Debug --target test_gameplay_nodes && ctest --test-dir build-vk -C Debug -R test_gameplay_nodes --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/gameplay/GameContext.h engine/gameplay/GameplayNodes.h engine/gameplay/GameplayNodes.cpp engine/CMakeLists.txt tests/test_gameplay_nodes.cpp tests/CMakeLists.txt
git commit -m "M55: gameplay node library (OnTick + Transform/Vec3/math/variable nodes)"
```

---

## Task 3: LogicGraph component + runtime

**Files:** Create `engine/gameplay/LogicGraph.h`, `engine/gameplay/LogicRuntime.h/.cpp`; modify `engine/world/ComponentArray.h`, `engine/CMakeLists.txt`, `tests/CMakeLists.txt`; test `tests/test_logic_runtime.cpp`.

- [ ] **Step 1: Add `forEach` iteration to `engine/world/ComponentArray.h`**

In `class ComponentArray`'s public section (after `size()`), add:
```cpp
    // Iterate live (entity, component) pairs. f: void(EntityId, T&).
    template <class F>
    void forEach(F&& f) {
        for (std::size_t i = 0; i < dense_.size(); ++i) f(denseEntities_[i], dense_[i]);
    }
```
(Confirm the member names `dense_` / `denseEntities_` match the file; adapt if different. Add `#include <cstddef>` if needed.)

- [ ] **Step 2: Create `engine/gameplay/LogicGraph.h`**

```cpp
#pragma once

#include "nodes/NodeGraph.h"

#include <string>
#include <unordered_map>

namespace iron {

// A logic graph attached to an entity. Each entity holds its own graph copy +
// persistent variables threaded across ticks.
struct LogicGraph {
    Graph graph;
    std::unordered_map<std::string, NodeValue> vars;
    bool started = false;   // reserved for a future OnStart event
};

}  // namespace iron
```

- [ ] **Step 3: Create `engine/gameplay/LogicRuntime.h`**

```cpp
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
```

- [ ] **Step 4: Write the failing test `tests/test_logic_runtime.cpp`**

```cpp
#include "gameplay/GameplayNodes.h"
#include "gameplay/LogicGraph.h"
#include "gameplay/LogicRuntime.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

using namespace iron;

int main() {
    NodeRegistry reg; registerBuiltinNodes(reg); registerGameplayNodes(reg);

    // A move graph: OnTick -> Translate by (1,0,0) each tick. After 5 ticks,
    // position.x == 5.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(tr, "delta", NodeValue::V3(Vec3{1.0f, 0.0f, 0.0f}));
        g.connect(tick, "then", tr, "in");
        w.add<LogicGraph>(e, LogicGraph{g, {}, false});

        for (int i = 0; i < 5; ++i) tickLogicGraphs(w, reg, i * 0.1f, 0.1f);
        CHECK_NEAR(w.get<Transform>(e)->position.x, 5.0f);
    }

    // Persistent variable accumulates across ticks: SetVar("n", GetVar("n")+1).
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getv = g.addNode("GetVar");
        const NodeId add  = g.addNode("Add");
        const NodeId setv = g.addNode("SetVar");
        g.setLiteral(getv, "name", NodeValue::S("n"));
        g.setLiteral(add, "b", NodeValue::F(1.0f));
        g.setLiteral(setv, "name", NodeValue::S("n"));
        g.connect(getv, "value", add, "a");
        g.connect(add, "result", setv, "value");
        g.connect(tick, "then", setv, "in");
        w.add<LogicGraph>(e, LogicGraph{g, {}, false});

        for (int i = 0; i < 3; ++i) tickLogicGraphs(w, reg, 0.0f, 0.1f);
        // After 3 ticks n == 3 (persisted on the component).
        const LogicGraph* lg = w.get<LogicGraph>(e);
        CHECK_NEAR(lg->vars.at("n").asFloat(), 3.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 5: Register source + test, build to confirm RED**

In `engine/CMakeLists.txt`, after `gameplay/GameplayNodes.cpp`, add:
```cmake
  gameplay/LogicRuntime.cpp
```
In `tests/CMakeLists.txt`, after `iron_add_test(test_gameplay_nodes test_gameplay_nodes.cpp)`:
```cmake
iron_add_test(test_logic_runtime test_logic_runtime.cpp)
```
Run: `cmake --build build-vk --config Debug --target test_logic_runtime`
Expected: FAIL — `tickLogicGraphs` undefined.

- [ ] **Step 6: Implement `engine/gameplay/LogicRuntime.cpp`**

```cpp
#include "gameplay/LogicRuntime.h"

#include "gameplay/GameContext.h"
#include "gameplay/LogicGraph.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeContext.h"
#include "world/World.h"

#include <utility>

namespace iron {

void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime) {
    world.view<LogicGraph>().forEach([&](EntityId e, LogicGraph& lg) {
        GameContext gc{&world, e, time, deltaTime};
        RunContext ctx;
        ctx.vars = std::move(lg.vars);     // restore persistent state
        ctx.domainContext = &gc;
        run(lg.graph, registry, ctx);
        lg.vars = std::move(ctx.vars);     // keep mutated state for next tick
    });
}

}  // namespace iron
```
> v1 gameplay nodes only touch `Transform`, never add/remove `LogicGraph`, so iterating `view<LogicGraph>()` while running is safe (no iterator invalidation). Spawn/destroy is out of scope.

- [ ] **Step 7: Build + run, then full sweep**

Run: `cmake --build build-vk --config Debug --target test_logic_runtime && ctest --test-dir build-vk -C Debug -R test_logic_runtime --output-on-failure`
Expected: PASS.
Then: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all green.

- [ ] **Step 8: Commit**

```bash
git add engine/world/ComponentArray.h engine/gameplay/LogicGraph.h engine/gameplay/LogicRuntime.h engine/gameplay/LogicRuntime.cpp engine/CMakeLists.txt tests/test_logic_runtime.cpp tests/CMakeLists.txt
git commit -m "M55: LogicGraph component + tickLogicGraphs runtime + ComponentArray::forEach"
```

---

## Task 4: SceneEntity logic-graph field + scene IO

**Files:** Modify `engine/scene/SceneFormat.h`, `engine/scene/SceneIO.cpp`; test `tests/test_scene_io.cpp`.

- [ ] **Step 1: Add the field to `SceneEntity` in `engine/scene/SceneFormat.h`**

In `struct SceneEntity`, after the existing optional component fields, add:
```cpp
    std::string logicGraph;   // M55 — serialized node graph (empty = none)
```

- [ ] **Step 2: Serialize it in `engine/scene/SceneIO.cpp`**

In `entityToJson(...)`, after the `if (e.probe) ...` line, add:
```cpp
    if (!e.logicGraph.empty()) j["logicGraph"] = e.logicGraph;
```
In `entityFromJson(...)`, after the probe block, add:
```cpp
    readString(j, "logicGraph", e.logicGraph);
```
(`readString` is the existing helper used for `name`; confirm its signature in the file and match it.)

- [ ] **Step 3: Add a round-trip test to `tests/test_scene_io.cpp`**

Find an existing scene-save/load round-trip test in the file and add a `logicGraph` assertion, OR add a focused block (adapt to the file's existing helpers for building/saving/loading a scene):
```cpp
    // M55: logicGraph string round-trips through save/load.
    {
        SceneFile scene;
        SceneEntity e;
        e.name = "scripted";
        e.logicGraph = R"({"nodes":[{"id":1,"type":"OnTick"}],"connections":[]})";
        scene.entities.push_back(e);

        const std::string path = "test_scene_logicgraph.json";
        CHECK(saveSceneFile(reflection, scene, path));   // match the file's save fn signature
        const auto loaded = loadSceneFile(reflection, path);
        CHECK(loaded.has_value());
        CHECK(loaded->entities.size() == 1);
        CHECK(loaded->entities[0].logicGraph == e.logicGraph);
    }
```
> Read `tests/test_scene_io.cpp` first: reuse its `Reflection`/scene-build/save-load helpers and exact `saveSceneFile`/`loadSceneFile` signatures (they take a `Reflection&`). Adapt the block to match.

- [ ] **Step 4: Build + run**

Run: `cmake --build build-vk --config Debug --target test_scene_io && ctest --test-dir build-vk -C Debug -R test_scene_io --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp
git commit -m "M55: SceneEntity.logicGraph field round-trips through scene IO"
```

---

## Task 5: Host integration — assign, build, Play tick, demo

**Files:** Modify `engine/editor/NodeGraphPanel.h/.cpp`, `games/11-sandbox/main.cpp`. Visual-gated.

> Read `games/11-sandbox/main.cpp` first: the scene→World build sites (where `world.add<iron::Transform>(...)` etc. happen, ~lines 329/393/465/899), the Play section (`if (editor.isPlaying())`, ~line 1174), the node-editor construction (~line 745), and how `selectedIndex` (the selected scene entity) is tracked.

- [ ] **Step 1: Add an "Assign to selected entity" intent to the panel**

In `engine/editor/NodeGraphPanel.h`, change `draw` to return whether the user clicked Assign:
```cpp
    // Returns true on the frame the "Assign to entity" button is clicked; the
    // host then copies the current graph onto the selected SceneEntity.
    bool draw(GraphEditorModel& model);
```
In `NodeGraphPanel.cpp`, in the toolbar (near Save/Load), add a button and return its state:
```cpp
    bool assignClicked = false;
    ImGui::SameLine();
    if (ImGui::Button("Assign to entity")) assignClicked = true;
```
and `return assignClicked;` at the end of `draw` (the function currently returns void — change the signature + add the return; the `ImGui::End()` stays before the return).

- [ ] **Step 2: Host: register gameplay nodes + use the Assign intent + build the registry once**

In `games/11-sandbox/main.cpp`, where `nodeRegistry` is built (~line 745-746), also register gameplay nodes:
```cpp
    iron::registerGameplayNodes(nodeRegistry);
```
Add includes near the others:
```cpp
#include "gameplay/GameplayNodes.h"
#include "gameplay/LogicGraph.h"
#include "gameplay/LogicRuntime.h"
```
Where the panel is drawn (~line 1186, `nodeGraphPanel.draw(graphModel);`), capture the intent and assign to the selected entity:
```cpp
        if (nodeGraphPanel.draw(graphModel)) {
            if (selectedIndex >= 0 && selectedIndex < (int)scene.entities.size())
                scene.entities[selectedIndex].logicGraph = graphModel.toJson().dump();
        }
```

- [ ] **Step 3: Host: add a LogicGraph component when building the World on Play**

At EACH scene→World build site that adds `Transform`/`MeshRef` (search for `world.add<iron::Transform>` — there are a few; the Play-mode one is the key one, but apply to the build path that runs on Edit→Play), after the existing `world.add<...>` calls for that entity, add:
```cpp
            if (!scene.entities[idx].logicGraph.empty()) {
                if (auto pg = iron::fromJson(
                        nlohmann::json::parse(scene.entities[idx].logicGraph,
                                              nullptr, false),
                        nodeRegistry)) {
                    iron::LogicGraph lg; lg.graph = std::move(*pg);
                    world.add<iron::LogicGraph>(entity, std::move(lg));
                }
            }
```
(Use the `idx`/`entity` variable names in scope at that build site; `nlohmann::json::parse(..., nullptr, false)` returns a discarded value on parse error → `fromJson` then returns nullopt → skipped. Include `"nodes/NodeGraphIO.h"` for `fromJson`.)

- [ ] **Step 4: Host: tick the logic graphs each frame while playing**

In the `if (editor.isPlaying())` per-frame section (~line 1174), add a call to advance all logic graphs. You need a Play-time accumulator + the frame dt (the sandbox already computes a frame delta — reuse it; otherwise accumulate from the frame time). Add near the other play-mode state (~line 591) a `float playTime = 0.0f;`, reset it to 0 in `togglePlayMode` on Edit→Play, and in the playing section:
```cpp
            playTime += frameDt;   // use the sandbox's existing per-frame delta
            iron::tickLogicGraphs(world, nodeRegistry, playTime, frameDt);
```
> Find the existing per-frame delta variable in main.cpp (grep for `frameDt`/`dt`/`glfwGetTime`); reuse it rather than inventing one. Call `tickLogicGraphs` BEFORE the world is rendered so movement shows the same frame.

- [ ] **Step 5: Seed a demo scripted entity**

Where the scene is initially built (the default/sample scene the sandbox loads), give one cube a bob graph so the gate has something immediately. After the scene's entities are created, set one entity's `logicGraph` to a bob program built via a `GraphEditorModel` (registry with builtins+gameplay) and serialized, OR a hand-written JSON string. Simplest — build it programmatically once at startup:
```cpp
    // M55 demo: make the first entity bob on Y via a node graph.
    if (!scene.entities.empty()) {
        iron::GraphEditorModel demo(&nodeRegistry);
        const auto tick = demo.addNode("OnTick", 40, 40);
        const auto mt   = demo.addNode("Mul", 40, 140);      // time * speed
        const auto sn   = demo.addNode("Sin", 240, 140);
        const auto getp = demo.addNode("GetPosition", 40, 260);
        const auto br   = demo.addNode("BreakVec3", 240, 260);
        const auto mk   = demo.addNode("MakeVec3", 460, 200);
        const auto setp = demo.addNode("SetPosition", 680, 60);
        demo.setLiteral(mt, "b", iron::NodeValue::F(2.0f));   // speed
        demo.connect(tick, "time", mt, "a");
        demo.connect(mt, "result", sn, "x");
        demo.connect(getp, "pos", br, "v");
        demo.connect(br, "x", mk, "x");
        demo.connect(sn, "result", mk, "y");                  // y = sin(time*speed)
        demo.connect(br, "z", mk, "z");
        demo.connect(mk, "v", setp, "pos");
        demo.connect(tick, "then", setp, "in");
        scene.entities[0].logicGraph = demo.toJson().dump();
    }
```
(Place after the scene is loaded/built and `nodeRegistry` exists. This makes entity 0 oscillate Y between -1 and 1 in Play. Adjust which entity / amplitude as desired.)

- [ ] **Step 6: Build the sandbox**

Run: `cmake --build build-vk --config Debug --target sandbox`
Expected: links cleanly.

- [ ] **Step 7: Full build + sweep**

Run: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all green.

- [ ] **Step 8: Commit**

```bash
git add engine/editor/NodeGraphPanel.h engine/editor/NodeGraphPanel.cpp games/11-sandbox/main.cpp
git commit -m "M55: sandbox runs entity logic graphs in Play + editor Assign + bob demo"
```

---

## Task 6: Update progress memory

- [ ] After the PR is opened, append an M55 entry to `iron-core-engine-progress.md` (gameplay nodes/runtime summary, the domainContext bridge, files, node-track position, PR number) + refresh the `MEMORY.md` index line. Documentation only.

---

## Visual Gate (after Task 5, with the user)

Launch the sandbox. Entity 0 sits still in Edit. Press **Play** → it **bobs up and down** (driven by the seeded `OnTick→Sin→SetPosition` graph). Press **Stop** → it returns to its start position (M41 snapshot). Then: select an entity, author a different graph in the Node Editor (e.g. `OnTick→Translate`), click **Assign to entity**, Play → that entity moves too. Iterate amplitude/speed at the gate.

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- `domainContext` bridge + `isEntry` entry generalization → Task 1. ✓
- `GameContext` + gameplay node set (OnTick/GetPosition/SetPosition/Translate/MakeVec3/BreakVec3/Mul/Sin/GetVar/SetVar) → Task 2. ✓
- `LogicGraph` component + `tickLogicGraphs` + `ComponentArray::forEach` → Task 3. ✓
- `SceneEntity.logicGraph` + scene IO round-trip → Task 4. ✓
- Editor "Assign" + scene→World build + Play tick + demo → Task 5. ✓
- Tests: gameplay nodes (incl. null-domainContext no-op), runtime (movement + var persistence), scene-IO round-trip → Tasks 2–4. ✓
- Out-of-scope (events/spawn/OnStart/non-Transform components) not implemented. ✓

**2. Placeholder scan:** Tasks 1–4 have full code. Task 5 (host integration) gives concrete code but flags discovery points (the exact frame-delta variable, the build-site `idx`/`entity` names, the `readString` signature) that the implementer binds against the real `main.cpp`/`SceneIO.cpp` — unavoidable for in-place edits to a large existing file, and flagged in the Risk note.

**3. Type consistency:** `RunContext::domainContext`, `NodeTypeDesc::isEntry`, `GameContext{world,self,time,deltaTime}`, `registerGameplayNodes`, `LogicGraph{graph,vars,started}`, `tickLogicGraphs(World&,const NodeRegistry&,float,float)`, `ComponentArray::forEach`, `SceneEntity::logicGraph` are used consistently across tasks. Reuses merged M53/M54 APIs (`NodeRegistry::registerType`/`find`, `PortDesc`/`PortDir`/`PortType`, `NodeValue::{F,S,V3,asFloat,asString,asVec3}`, `iron::run`, `iron::fromJson`, `GraphEditorModel::{addNode,connect,setLiteral,toJson}`, `Graph::{addNode,connect,setLiteral,disconnect}`) and world APIs (`World::{create,add,get,view}`, `Transform::position`, `EntityId`, `Vec3` `operator+`) verified against the headers read during planning. ✓
