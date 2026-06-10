# M68 — Components in Node Graphs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Auto-generate `Get <Component>` / `Set <Component> <field>` node types from the ComponentRegistry so logic graphs can read/write reflected component fields on their own entity during Play.

**Architecture:** A new `registerComponentNodes(NodeRegistry&, const ComponentRegistry&)` generates ordinary static-port `NodeTypeDesc`s at startup (zero node-core changes). Nodes resolve self-only through the existing `GameContext` against a `ComponentSet` mirrored into the `World` on Edit→Play; Stop's snapshot restore discards all writes. Plus: `FieldMeta.readOnly`, a `Health` demo component, and a sinking-cube demo graph.

**Tech Stack:** C++20, existing `engine/nodes` + `engine/reflection` + `engine/world` systems. Build dir `build-vk` (Vulkan, multi-config MSVC). Tests via CTest (`-C Debug`).

**Spec:** `docs/superpowers/specs/2026-06-09-m68-components-in-node-graphs-design.md`
**Branch:** `m68-components-in-node-graphs` (already created; spec committed).

**Conventions for every task:**
- Run tests with `ctest --test-dir build-vk -C Debug -R <name> --output-on-failure`. Verify the **exit code**, not the truncated tail.
- Configure happens once; new source files need a CMake reconfigure (building the target does this automatically with the VS generator).
- Commit messages end with the trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` (no other trailer).

---

## File map

| File | Action | Responsibility |
|---|---|---|
| `engine/reflection/FieldDesc.h` | Modify | add `FieldMeta.readOnly` |
| `tests/test_reflection.cpp` | Modify | readOnly registration/retrieval check |
| `engine/gameplay/LogicGraphComponent.reflect.cpp` | Modify | mark `graph` readOnly (belt + braces over hidden) |
| `engine/gameplay/Health.h` | Create | demo POD component `{current, max}` |
| `engine/gameplay/Health.reflect.cpp` | Create | `registerHealth(Reflection&)` sidecar |
| `engine/reflection/RegisterCoreTypes.h` | Modify | declare `registerHealth` |
| `engine/scene/RegisterCoreComponents.cpp` | Modify | register Health in the ComponentRegistry |
| `tests/test_core_components.cpp` | Modify | Health in registry; count 4 → 5 |
| `tests/test_scene_io.cpp` | Modify | `registerHealth` in `makeReflectionRegistry` |
| `engine/gameplay/ComponentNodes.h` | Create | `registerComponentNodes` API |
| `engine/gameplay/ComponentNodes.cpp` | Create | Get/Set node generation + TypeId↔NodeValue bridging |
| `tests/test_component_nodes.cpp` | Create | generation rules + evaluator round-trips + guard paths |
| `engine/CMakeLists.txt` | Modify | add ComponentNodes.cpp + Health.reflect.cpp |
| `tests/CMakeLists.txt` | Modify | add test_component_nodes |
| `games/11-sandbox/main.cpp` | Modify | registerHealth + registerComponentNodes + ComponentSet mirror + demo graph |

---

### Task 1: `FieldMeta.readOnly`

**Files:**
- Modify: `engine/reflection/FieldDesc.h` (FieldMeta, ~line 12-19)
- Modify: `tests/test_reflection.cpp`
- Modify: `engine/gameplay/LogicGraphComponent.reflect.cpp:8`

- [ ] **Step 1: Write the failing test**

Append this block inside `main()` of `tests/test_reflection.cpp`, just before `return iron_test_result();`. The file already has `using namespace iron;` (same pattern as sibling tests) — if it instead qualifies `iron::` explicitly, match that style.

```cpp
    // M68: FieldMeta.readOnly is registered and retrievable.
    {
        struct RoProbe { float locked = 0.0f; float open = 0.0f; };
        Reflection rr;
        rr.registerType<RoProbe>("RoProbe")
            .field("locked", &RoProbe::locked, {.readOnly = true})
            .field("open",   &RoProbe::open);
        CHECK(rr.fieldByName<RoProbe>("locked")->meta.readOnly);
        CHECK(!rr.fieldByName<RoProbe>("open")->meta.readOnly);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --config Debug --target test_reflection`
Expected: **compile error** — `'readOnly': is not a member of 'iron::FieldMeta'`.

- [ ] **Step 3: Add the flag**

In `engine/reflection/FieldDesc.h`, add to `FieldMeta` after the `hidden` member (declaration order matters for designated initializers — `readOnly` goes LAST):

```cpp
    bool  hidden    = false;  // skip in the Inspector (still serialized — IO ignores meta)
    bool  readOnly  = false;  // M68: no Set node generated; Inspector may grey out later
```

Also mark `LogicGraphComponent::graph` in `engine/gameplay/LogicGraphComponent.reflect.cpp` (already hidden — which alone excludes it from node generation — but readOnly documents intent and guards if it's ever un-hidden):

```cpp
        .field("graph", &LogicGraphComponent::graph, {.hidden = true, .readOnly = true});
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-vk --config Debug --target test_reflection` then
`ctest --test-dir build-vk -C Debug -R test_reflection --output-on-failure`
Expected: PASS (exit code 0).

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/FieldDesc.h engine/gameplay/LogicGraphComponent.reflect.cpp tests/test_reflection.cpp
git commit -m "M68: FieldMeta.readOnly flag (Set-node generation will skip these)"
```

---

### Task 2: Health demo component

**Files:**
- Create: `engine/gameplay/Health.h`
- Create: `engine/gameplay/Health.reflect.cpp`
- Modify: `engine/reflection/RegisterCoreTypes.h` (declaration list, ~line 14)
- Modify: `engine/scene/RegisterCoreComponents.cpp`
- Modify: `engine/CMakeLists.txt` (next to `gameplay/LogicGraphComponent.reflect.cpp`, ~line 51)
- Modify: `tests/test_core_components.cpp`
- Modify: `tests/test_scene_io.cpp:36` (`makeReflectionRegistry`)
- Modify: `games/11-sandbox/main.cpp:175` (Reflection registration block)

- [ ] **Step 1: Write the failing test**

In `tests/test_core_components.cpp`:

Add `registerHealth(r);` after `registerLogicGraphComponent(r);` (line 18). Then update the assertions:

```cpp
    CHECK(reg.byName("CollisionShape")     != nullptr);
    CHECK(reg.byName("AudioEmitter")       != nullptr);
    CHECK(reg.byName("ReflectionProbeDef") != nullptr);
    CHECK(reg.byName("LogicGraphComponent")!= nullptr);
    CHECK(reg.byName("Health")             != nullptr);            // M68
    CHECK(reg.byTypeId(componentTypeId<CollisionShape>()) != nullptr);
    CHECK(reg.order().size() == 5u);                               // was 4

    // LogicGraphComponent has its one reflected string field.
    CHECK(reg.byName("LogicGraphComponent")->fields.size() == 1u);

    // M68: Health has current + max, neither hidden nor readOnly.
    CHECK(reg.byName("Health")->fields.size() == 2u);
    CHECK(!reg.byName("Health")->fields[0].meta.readOnly);
```

No new include needed — `registerHealth` will be declared in the already-included `reflection/RegisterCoreTypes.h`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --config Debug --target test_core_components`
Expected: **compile error** — `'registerHealth': identifier not found`.

- [ ] **Step 3: Implement**

Create `engine/gameplay/Health.h`:

```cpp
#pragma once

namespace iron {

// M68 demo gameplay component: a drainable health pool. Standard-layout POD
// so Reflection, SceneIO, the Inspector, and component-node generation all
// handle it with zero bespoke code (registering it below is the whole job).
struct Health {
    float current = 100.0f;
    float max     = 100.0f;
};

}  // namespace iron
```

Create `engine/gameplay/Health.reflect.cpp`:

```cpp
#include "gameplay/Health.h"
#include "reflection/Reflection.h"

namespace iron {

void registerHealth(Reflection& r) {
    r.registerType<Health>("Health")
        .field("current", &Health::current)
        .field("max",     &Health::max);
}

}  // namespace iron
```

In `engine/reflection/RegisterCoreTypes.h`, add after the `registerLogicGraphComponent` declaration:

```cpp
void registerHealth(Reflection& r);
```

In `engine/scene/RegisterCoreComponents.cpp`, add the include + registration:

```cpp
#include "gameplay/Health.h"
```

```cpp
    cr.registerComponent<LogicGraphComponent>("LogicGraphComponent", r);
    cr.registerComponent<Health>("Health", r);   // M68 demo gameplay component
```

In `engine/CMakeLists.txt`, after `gameplay/LogicGraphComponent.reflect.cpp` (line 51):

```cmake
  gameplay/Health.reflect.cpp
```

Keep all `registerCoreComponents` callers correct (they must register Health in Reflection FIRST, else its registry entry captures an empty field span):
- `tests/test_scene_io.cpp` `makeReflectionRegistry()` — add `iron::registerHealth(r);` after `iron::registerLogicGraphComponent(r);` (line 36).
- `games/11-sandbox/main.cpp` — add `iron::registerHealth(reflection);` after `iron::registerLogicGraphComponent(reflection);` (line 175).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build-vk --config Debug --target test_core_components --target test_scene_io --target sandbox`
then `ctest --test-dir build-vk -C Debug -R "test_core_components|test_scene_io" --output-on-failure`
Expected: PASS (exit code 0), sandbox builds clean.

- [ ] **Step 5: Commit**

```bash
git add engine/gameplay/Health.h engine/gameplay/Health.reflect.cpp engine/reflection/RegisterCoreTypes.h engine/scene/RegisterCoreComponents.cpp engine/CMakeLists.txt tests/test_core_components.cpp tests/test_scene_io.cpp games/11-sandbox/main.cpp
git commit -m "M68: Health demo component (one registration call, everywhere generic)"
```

---

### Task 3: ComponentNodes — generated Get nodes

**Files:**
- Create: `engine/gameplay/ComponentNodes.h`
- Create: `engine/gameplay/ComponentNodes.cpp`
- Create: `tests/test_component_nodes.cpp`
- Modify: `engine/CMakeLists.txt` (next to `gameplay/GameplayNodes.cpp`, ~line 49)
- Modify: `tests/CMakeLists.txt` (next to `test_gameplay_nodes`, ~line 27)

- [ ] **Step 1: Write the failing test**

Create `tests/test_component_nodes.cpp` (Get coverage; Set coverage lands in Task 4):

```cpp
#include "gameplay/ComponentNodes.h"
#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "math/Quat.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "reflection/Reflection.h"
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "world/World.h"
#include "test_framework.h"

#include <string>

using namespace iron;

namespace {

// One field per supported TypeId + a readOnly + a hidden + an unsupported one.
struct Combat {
    float       power  = 10.0f;
    int         ammo   = 3;
    bool        armed  = true;
    Vec3        aim    = Vec3{0.0f, 0.0f, 1.0f};
    std::string label  = "gun";
    float       score  = 1.5f;   // readOnly -> Get pin, no Set node
    float       secret = 9.0f;   // hidden   -> no pins, no nodes
    Quat        facing = {};     // Quat unsupported -> no pin, no node
};

void registerCombat(Reflection& r) {
    r.registerType<Combat>("Combat")
        .field("power",  &Combat::power)
        .field("ammo",   &Combat::ammo)
        .field("armed",  &Combat::armed)
        .field("aim",    &Combat::aim)
        .field("label",  &Combat::label)
        .field("score",  &Combat::score,  {.readOnly = true})
        .field("secret", &Combat::secret, {.hidden = true})
        .field("facing", &Combat::facing);
}

const PortDesc* findPort(const NodeTypeDesc* d, std::string_view name) {
    for (const PortDesc& p : d->ports)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace

int main() {
    Reflection r;
    registerCombat(r);
    ComponentRegistry cr;
    cr.registerComponent<Combat>("Combat", r);

    NodeRegistry reg;
    registerBuiltinNodes(reg);
    registerGameplayNodes(reg);
    registerComponentNodes(reg, cr);

    // Get node exists with `has` + exactly the supported, non-hidden fields.
    {
        const NodeTypeDesc* d = reg.find("Get Combat");
        CHECK(d != nullptr);
        CHECK(d->category == "Components");
        CHECK(d->ports.size() == 7u);   // has + power/ammo/armed/aim/label/score
        CHECK(findPort(d, "has")   && findPort(d, "has")->type   == PortType::Bool);
        CHECK(findPort(d, "power") && findPort(d, "power")->type == PortType::Float);
        CHECK(findPort(d, "ammo")  && findPort(d, "ammo")->type  == PortType::Int);
        CHECK(findPort(d, "armed") && findPort(d, "armed")->type == PortType::Bool);
        CHECK(findPort(d, "aim")   && findPort(d, "aim")->type   == PortType::Vec3);
        CHECK(findPort(d, "label") && findPort(d, "label")->type == PortType::String);
        CHECK(findPort(d, "score") && findPort(d, "score")->type == PortType::Float);
        CHECK(findPort(d, "secret") == nullptr);   // hidden
        CHECK(findPort(d, "facing") == nullptr);   // unsupported Quat
        for (const PortDesc& p : d->ports) CHECK(p.dir == PortDir::Out);
    }

    // Get reads live values from the entity's runtime ComponentSet.
    {
        World w; EntityId e = w.create();
        ComponentSet cs;
        Combat c0; c0.power = 77.0f;
        cs.add<Combat>(c0);
        w.add<ComponentSet>(e, cs);

        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(out, "key", NodeValue::S("p"));
        g.connect(getN, "power", out, "value");
        g.connect(tick, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 77.0f);
    }

    // Missing component: has=false, field pins read type-appropriate zeros.
    {
        World w; EntityId e = w.create();   // no ComponentSet at all
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId outH = g.addNode("SetOutput");
        const NodeId outP = g.addNode("SetOutput");
        const NodeId seq  = g.addNode("Sequence");
        g.setLiteral(outH, "key", NodeValue::S("has"));
        g.setLiteral(outP, "key", NodeValue::S("p"));
        g.connect(getN, "has",   outH, "value");
        g.connect(getN, "power", outP, "value");
        g.connect(tick, "then", seq, "in");
        g.connect(seq, "0", outH, "in");
        g.connect(seq, "1", outP, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK(!ctx.outputs.at("has").asBool());
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 0.0f);
    }

    // Null domainContext (editor Run preview): no crash, has=false.
    {
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(out, "key", NodeValue::S("has"));
        g.connect(getN, "has", out, "value");
        g.connect(tick, "then", out, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);                    // must not crash
        CHECK(!ctx.outputs.at("has").asBool());
    }

    return iron_test_result();
}
```

Add to `tests/CMakeLists.txt` after `iron_add_test(test_gameplay_nodes test_gameplay_nodes.cpp)`:

```cmake
iron_add_test(test_component_nodes test_component_nodes.cpp)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --config Debug --target test_component_nodes`
Expected: **compile error** — `gameplay/ComponentNodes.h` not found.

- [ ] **Step 3: Implement Get generation**

Create `engine/gameplay/ComponentNodes.h`:

```cpp
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
// pin and no Set node. Nodes resolve self-only via GameContext against
// world->get<ComponentSet>(self); missing component / null context => Get
// reports has=false + zeros, Set is a silent no-op, exec always continues.
// Call once at startup, after registerCoreComponents + registerGameplayNodes.
void registerComponentNodes(NodeRegistry& nodes, const ComponentRegistry& components);

}  // namespace iron
```

Create `engine/gameplay/ComponentNodes.cpp` (Set generation is Task 4 — this step ships Get only):

```cpp
#include "gameplay/ComponentNodes.h"

#include "gameplay/GameContext.h"
#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"
#include "reflection/FieldDesc.h"
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "world/World.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iron {
namespace {

// TypeId -> PortType for the v1 supported set. nullopt = field gets no pin.
std::optional<PortType> portTypeFor(TypeId t) {
    switch (t) {
        case TypeId::Float:  return PortType::Float;
        case TypeId::Int32:  return PortType::Int;
        case TypeId::Bool:   return PortType::Bool;
        case TypeId::Vec3:   return PortType::Vec3;
        case TypeId::String: return PortType::String;
        default:             return std::nullopt;   // Quat/Enum/UInt*/... skipped
    }
}

// Resolve the running graph's self entity -> its runtime ComponentSet -> the
// component instance of `typeId`. nullptr on any miss (null domainContext in
// the editor preview, dead entity, no ComponentSet, component absent).
void* componentOf(NodeContext& c, std::uint32_t typeId) {
    auto* g = static_cast<GameContext*>(c.run().domainContext);
    if (!g || !g->world) return nullptr;
    ComponentSet* set = g->world->get<ComponentSet>(g->self);
    if (!set) return nullptr;
    for (const auto& box : set->all())
        if (box->typeId() == typeId) return box->data();
    return nullptr;
}

NodeValue readField(const void* obj, const FieldDesc& f) {
    switch (f.type) {
        case TypeId::Float:  return NodeValue::F(*f.ptr<float>(obj));
        case TypeId::Int32:  return NodeValue::I(*f.ptr<std::int32_t>(obj));
        case TypeId::Bool:   return NodeValue::B(*f.ptr<bool>(obj));
        case TypeId::Vec3:   return NodeValue::V3(*f.ptr<Vec3>(obj));
        case TypeId::String: return NodeValue::S(*f.ptr<std::string>(obj));
        default:             return NodeValue{};   // unreachable: pins are pre-filtered
    }
}

}  // namespace

void registerComponentNodes(NodeRegistry& nodes, const ComponentRegistry& components) {
    for (std::uint32_t typeId : components.order()) {
        const ComponentRegistry::Entry* e = components.byTypeId(typeId);
        if (!e) continue;
        const std::string compName(e->name);

        // Supported, non-hidden fields. Copies are safe: FieldDesc is a small
        // POD whose string_view name points at a static string literal.
        std::vector<FieldDesc> gettable;
        std::string subtitle;
        for (const FieldDesc& f : e->fields) {
            if (f.meta.hidden) continue;
            if (!portTypeFor(f.type)) continue;
            if (f.name == "has") continue;   // reserved for the Get node's has pin
            gettable.push_back(f);
            if (!subtitle.empty()) subtitle += ", ";
            subtitle += f.name;
        }

        // "Get <Component>": has + one output pin per field.
        NodeTypeDesc d;
        d.typeName = "Get " + compName;
        d.category = "Components";
        d.subtitle = subtitle;
        d.ports.push_back({"has", PortType::Bool, PortDir::Out});
        for (const FieldDesc& f : gettable)
            d.ports.push_back({std::string(f.name), *portTypeFor(f.type), PortDir::Out});
        d.evaluate = [typeId, gettable](NodeContext& c) {
            const void* obj = componentOf(c, typeId);
            c.out("has", NodeValue::B(obj != nullptr));
            for (const FieldDesc& f : gettable)
                c.out(f.name, obj ? readField(obj, f)
                                  : zeroValue(*portTypeFor(f.type)));
        };
        nodes.registerType(std::move(d));
    }
}

}  // namespace iron
```

Add to `engine/CMakeLists.txt` after `gameplay/GameplayNodes.cpp` (line 49):

```cmake
  gameplay/ComponentNodes.cpp
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-vk --config Debug --target test_component_nodes`
then `ctest --test-dir build-vk -C Debug -R test_component_nodes --output-on-failure`
Expected: PASS (exit code 0).

- [ ] **Step 5: Commit**

```bash
git add engine/gameplay/ComponentNodes.h engine/gameplay/ComponentNodes.cpp tests/test_component_nodes.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M68: generated Get <Component> nodes from the ComponentRegistry"
```

---

### Task 4: ComponentNodes — generated Set nodes

**Files:**
- Modify: `engine/gameplay/ComponentNodes.cpp`
- Modify: `tests/test_component_nodes.cpp`

- [ ] **Step 1: Write the failing tests**

Append inside `main()` of `tests/test_component_nodes.cpp`, before `return iron_test_result();`:

```cpp
    // Set nodes exist for exactly the writable supported fields.
    {
        CHECK(reg.find("Set Combat power") != nullptr);
        CHECK(reg.find("Set Combat ammo")  != nullptr);
        CHECK(reg.find("Set Combat armed") != nullptr);
        CHECK(reg.find("Set Combat aim")   != nullptr);
        CHECK(reg.find("Set Combat label") != nullptr);
        CHECK(reg.find("Set Combat score")  == nullptr);   // readOnly
        CHECK(reg.find("Set Combat secret") == nullptr);   // hidden
        CHECK(reg.find("Set Combat facing") == nullptr);   // unsupported Quat
        const NodeTypeDesc* d = reg.find("Set Combat aim");
        CHECK(d->category == "Components");
        CHECK(d->ports.size() == 3u);   // in / value / then
        CHECK(findPort(d, "in")    && findPort(d, "in")->type    == PortType::Exec);
        CHECK(findPort(d, "value") && findPort(d, "value")->type == PortType::Vec3);
        CHECK(findPort(d, "then")  && findPort(d, "then")->type  == PortType::Exec);
    }

    // Set-then-Get round-trip through the evaluator, per supported type.
    // Chain: OnTick -> Set power -> Set ammo -> Set armed -> Set aim ->
    // Set label -> SetOutput(reads Get Combat power post-write).
    {
        World w; EntityId e = w.create();
        ComponentSet cs; cs.add<Combat>();
        w.add<ComponentSet>(e, cs);

        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP = g.addNode("Set Combat power");
        const NodeId sA = g.addNode("Set Combat ammo");
        const NodeId sB = g.addNode("Set Combat armed");
        const NodeId sV = g.addNode("Set Combat aim");
        const NodeId sS = g.addNode("Set Combat label");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(42.0f));
        g.setLiteral(sA, "value", NodeValue::I(9));
        g.setLiteral(sB, "value", NodeValue::B(false));
        g.setLiteral(sV, "value", NodeValue::V3(Vec3{1.0f, 2.0f, 3.0f}));
        g.setLiteral(sS, "value", NodeValue::S("sword"));
        g.setLiteral(out, "key", NodeValue::S("p"));
        g.connect(getN, "power", out, "value");
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", sA, "in");
        g.connect(sA, "then", sB, "in");
        g.connect(sB, "then", sV, "in");
        g.connect(sV, "then", sS, "in");
        g.connect(sS, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);

        // Get pulled AFTER the writes in the exec chain sees the new value.
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 42.0f);
        // And the runtime World copy holds every written value.
        Combat* c = w.get<ComponentSet>(e)->get<Combat>();
        CHECK(c != nullptr);
        CHECK_NEAR(c->power, 42.0f);
        CHECK(c->ammo == 9);
        CHECK(!c->armed);
        CHECK_NEAR(c->aim.x, 1.0f);
        CHECK_NEAR(c->aim.y, 2.0f);
        CHECK_NEAR(c->aim.z, 3.0f);
        CHECK(c->label == "sword");
        // readOnly + hidden + unsupported fields untouched (no Set node exists).
        CHECK_NEAR(c->score, 1.5f);
        CHECK_NEAR(c->secret, 9.0f);
    }

    // Missing component: Set is a silent no-op and exec continues past it.
    {
        World w; EntityId e = w.create();   // no ComponentSet
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP   = g.addNode("Set Combat power");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(5.0f));
        g.setLiteral(out, "key", NodeValue::S("ran"));
        g.setLiteral(out, "value", NodeValue::F(1.0f));
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ran").asFloat(), 1.0f);   // chain didn't stall
    }

    // Null domainContext: Set no-ops without crashing, exec continues.
    {
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP   = g.addNode("Set Combat power");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(5.0f));
        g.setLiteral(out, "key", NodeValue::S("ran"));
        g.setLiteral(out, "value", NodeValue::F(1.0f));
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", out, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ran").asFloat(), 1.0f);
    }

    // Generated nodes are part of the AI contract (catalogToJson).
    {
        const std::string cat = catalogToJson(reg).dump();
        CHECK(cat.find("Get Combat") != std::string::npos);
        CHECK(cat.find("Set Combat power") != std::string::npos);
        CHECK(cat.find("Set Combat score") == std::string::npos);   // readOnly
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --config Debug --target test_component_nodes`, then
`ctest --test-dir build-vk -C Debug -R test_component_nodes --output-on-failure`
Expected: FAIL — `reg.find("Set Combat power")` returns null (Set generation doesn't exist yet).

- [ ] **Step 3: Implement Set generation**

In `engine/gameplay/ComponentNodes.cpp`, add `writeField` to the anonymous namespace (after `readField`):

```cpp
void writeField(void* obj, const FieldDesc& f, const NodeValue& v) {
    switch (f.type) {
        case TypeId::Float:  *f.ptr<float>(obj)        = v.asFloat();  break;
        case TypeId::Int32:  *f.ptr<std::int32_t>(obj) = v.asInt();    break;
        case TypeId::Bool:   *f.ptr<bool>(obj)         = v.asBool();   break;
        case TypeId::Vec3:   *f.ptr<Vec3>(obj)         = v.asVec3();   break;
        case TypeId::String: *f.ptr<std::string>(obj)  = v.asString(); break;
        default: break;   // unreachable: Set nodes are pre-filtered
    }
}
```

Then, inside `registerComponentNodes`'s per-component loop, after `nodes.registerType(std::move(d));` (the Get registration), add:

```cpp
        // "Set <Component> <field>": one node per writable field.
        for (const FieldDesc& f : gettable) {
            if (f.meta.readOnly) continue;
            NodeTypeDesc s;
            s.typeName = "Set " + compName + " " + std::string(f.name);
            s.category = "Components";
            s.subtitle = compName + "." + std::string(f.name);
            s.ports = { {"in", PortType::Exec, PortDir::In},
                        {"value", *portTypeFor(f.type), PortDir::In},
                        {"then", PortType::Exec, PortDir::Out} };
            s.evaluate = [typeId, f](NodeContext& c) {
                if (void* obj = componentOf(c, typeId))
                    writeField(obj, f, c.in("value"));
                c.fire("then");   // always continue, even on a no-op (M55 convention)
            };
            nodes.registerType(std::move(s));
        }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-vk --config Debug --target test_component_nodes`, then
`ctest --test-dir build-vk -C Debug -R test_component_nodes --output-on-failure`
Expected: PASS (exit code 0).

- [ ] **Step 5: Run the full suite (regression guard)**

Run: `cmake --build build-vk --config Debug`, then
`ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: ALL tests pass (exit code 0). Verify the exit code explicitly.

- [ ] **Step 6: Commit**

```bash
git add engine/gameplay/ComponentNodes.cpp tests/test_component_nodes.cpp
git commit -m "M68: generated Set <Component> <field> nodes (readOnly/hidden skipped)"
```

---

### Task 5: Sandbox wiring — registration, Play-mode ComponentSet mirror, demo graph

**Files:**
- Modify: `games/11-sandbox/main.cpp` (four spots; line numbers are pre-M68 references)

- [ ] **Step 1: Register the generated nodes**

After `iron::registerGameplayNodes(nodeRegistry);` (line 196), add:

```cpp
    iron::registerComponentNodes(nodeRegistry, componentRegistry);  // M68: Get/Set component fields
```

Add the include next to the other gameplay includes at the top of the file:

```cpp
#include "gameplay/ComponentNodes.h"
#include "gameplay/Health.h"
```

(`iron::registerHealth(reflection);` was already added at line ~176 in Task 2.)

- [ ] **Step 2: Mirror the ComponentSet into the World on Edit→Play**

In the `spawnRuntime` lambda (line 672), insert right after `const iron::SceneEntity& e = scene.entities[i];`:

```cpp
            // M68: mirror the authored component bag into the runtime World so
            // the generated component Get/Set nodes resolve against a
            // Play-only copy (discarded by the Stop snapshot restore).
            if (i < static_cast<int>(sceneIndexToEntity.size()))
                world.add<iron::ComponentSet>(sceneIndexToEntity[i], e.components);
```

No `despawnRuntime` change: Stop already restores `world = editWorld`, which drops the mirrored sets (same mechanism that drops M55's LogicGraph components).

- [ ] **Step 3: Replace the M55 bob demo with the M68 health-drain demo**

Replace the whole `// M55 demo: make the first entity bob on Y via a node graph.` block (lines 887–910, the `if (!scene.entities.empty()) { ... }` statement) with:

```cpp
    // M68 demo: entity 0 gets a Health component and a graph that drains it
    // and maps it onto Y — the cube visibly sinks as its health runs out.
    //   exec: OnTick -> Set Health current -> SetPosition
    //   data: current' = current + dt*(-10);  y = current * 0.05  (pre-write
    //         memoized read: one-frame lag, invisible at 60 fps)
    if (!scene.entities.empty()) {
        scene.entities[0].components.add<iron::Health>();
        iron::GraphEditorModel demo(&nodeRegistry);
        const auto tick   = demo.addNode("OnTick", 40, 40);
        const auto getH   = demo.addNode("Get Health", 40, 180);
        const auto drain  = demo.addNode("Mul", 240, 60);    // dt * -10
        const auto add    = demo.addNode("Add", 420, 100);   // current + drain
        const auto setH   = demo.addNode("Set Health current", 620, 40);
        const auto getp   = demo.addNode("GetPosition", 40, 320);
        const auto br     = demo.addNode("BreakVec3", 240, 320);
        const auto scaleY = demo.addNode("Mul", 420, 240);   // current * 0.05
        const auto mk     = demo.addNode("MakeVec3", 620, 280);
        const auto setp   = demo.addNode("SetPosition", 840, 60);
        demo.setLiteral(drain, "b", iron::NodeValue::F(-10.0f));
        demo.setLiteral(scaleY, "b", iron::NodeValue::F(0.05f));
        demo.connect(tick, "dt", drain, "a");
        demo.connect(getH, "current", add, "a");
        demo.connect(drain, "result", add, "b");
        demo.connect(add, "result", setH, "value");
        demo.connect(getH, "current", scaleY, "a");
        demo.connect(getp, "pos", br, "v");
        demo.connect(br, "x", mk, "x");
        demo.connect(scaleY, "result", mk, "y");
        demo.connect(br, "z", mk, "z");
        demo.connect(mk, "v", setp, "pos");
        demo.connect(tick, "then", setH, "in");
        demo.connect(setH, "then", setp, "in");
        scene.entities[0].components.add<iron::LogicGraphComponent>(
            iron::LogicGraphComponent{ demo.toJson().dump() });
    }
```

- [ ] **Step 4: Build + run the suite**

Run: `cmake --build build-vk --config Debug --target sandbox`, then the full
`ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: sandbox builds clean; ALL tests pass (exit code 0).

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M68: sandbox wiring — component nodes registered, ComponentSet Play mirror, health-drain demo"
```

---

### Task 6: Full verification + visual gate

**Files:** none (verification only)

- [ ] **Step 1: Clean-ish full build of every target**

Run: `cmake --build build-vk --config Debug`
Expected: exit code 0, no new warnings in M68-touched files. (CI does a truly clean build — see [[verify-clean-build-before-ci]]; if in doubt, configure+build a scratch dir.)

- [ ] **Step 2: Full test suite**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: ALL tests pass, including the new `test_component_nodes`. Verify exit code 0.

- [ ] **Step 3: Visual gate (user-run)**

Launch `build-vk/games/11-sandbox/Debug/sandbox.exe` and verify:
1. **Play (F5):** entity 0 jumps to y=5 (health 100 × 0.05) and sinks steadily toward y=0 over ~10 s as the graph drains Health. Stop: entity restores to its authored position.
2. **Node Editor:** select entity 0 → Load-from-entity → the drain graph renders with "Get Health" / "Set Health current" nodes; pins are typed (Float greens, Exec arrows).
3. **Create menu:** right-click canvas → search "health" → "Get Health" and "Set Health current"/"Set Health max" appear under Components.
4. **Inspector:** entity 0 shows the Health component (current/max editable); Add Component menu lists Health.
5. **Editor Run preview** (toolbar Run on a graph containing component nodes) does not crash (nodes no-op without a Play world).

- [ ] **Step 4: Wrap up**

After the gate passes: push the branch, open the PR (body ends with the Claude Code footer), let CI go green, squash-merge, update memory per [[iron-core-engine-progress]] conventions.

---

## Self-review notes (already applied)

- **Spec coverage:** decisions 1–5 map to Tasks 3/4 (node shape, types, readOnly), Task 5 (addressing + write model + demo), Task 1 (readOnly flag), Task 2 (Health). Error-handling table → Task 3/4 guard tests. Catalog requirement → Task 4 Step 1. Reflection retrieval test → Task 1.
- **Memoization caveat:** the demo's `scaleY` reads the pre-write `current` (Get Combat/Health is memoized per run) — documented in the demo comment; the round-trip test instead asserts the post-write read works because `Get` is first pulled *after* the Set chain ran.
- **Type consistency:** `registerComponentNodes(NodeRegistry&, const ComponentRegistry&)`, `portTypeFor`, `componentOf`, `readField`, `writeField`, port names `has`/`in`/`value`/`then`, category `"Components"` — used identically across Tasks 3–5.
- **Reflection lifetime:** ComponentRegistry entries hold `std::span` into the `Reflection` instance — in tests, `Reflection r` is declared before `ComponentRegistry cr` and both live for all of `main` (same pattern as `test_component_registry`).
