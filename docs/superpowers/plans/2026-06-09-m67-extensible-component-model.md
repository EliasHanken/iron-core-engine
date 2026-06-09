# M67 — Extensible Component Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `SceneEntity`'s hardcoded `std::optional<X>` component fields with a registry-driven, type-erased `ComponentSet`, so adding an authorable component type becomes one registration call (generic Add-Component menu + reflected editing + serialization).

**Architecture:** A `ComponentSet` (per-entity type-erased, deep-copyable bag) + a `ComponentRegistry` layered on the existing `Reflection`. The Inspector and SceneIO iterate the registry instead of hardcoded fields. Collision/Audio/Probe + a new `LogicGraphComponent` are migrated onto it.

**Tech Stack:** C++17, the existing `iron::Reflection` + `ReflectionIO` (field dispatch), `componentTypeId<T>()` (engine/world/Entity.h), nlohmann::json, ImGui (editor), the in-repo `test_framework.h`.

**Build dir:** `build-vk` (canonical, multi-config MSVC — ctest needs `-C Debug`). Always check the build EXIT CODE, not the truncated tail ([[verify-clean-build-before-ci]]). Kill stray `sandbox`/game exes before building if a link fails with LNK1168.

**Note on build staging:** Task 4 changes `SceneEntity`'s fields, which breaks `ironcore_editor` (SceneInspector) and `games/11-sandbox` until Tasks 5–6 update them. Tasks 4 builds/tests **only** `ironcore` + the test targets; Tasks 5/6 restore the editor and sandbox; Task 7 confirms a full clean build. This staged breakage is expected and called out per task.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `engine/world/ComponentSet.h` (create) | type-erased, deep-copyable per-entity component bag | 1 |
| `tests/test_component_set.cpp` (create) | ComponentSet unit tests | 1 |
| `engine/world/ComponentRegistry.h` (create) + `.cpp` | registry of component types on top of Reflection | 2 |
| `tests/test_component_registry.cpp` (create) | registry unit tests | 2 |
| `engine/gameplay/LogicGraphComponent.h` (create) | reflected wrapper for the logic-graph string | 3 |
| `engine/gameplay/LogicGraphComponent.reflect.cpp` (create) | its reflection registration | 3 |
| `engine/scene/RegisterCoreComponents.h` (create) + `.cpp` | populate a ComponentRegistry with the 4 core components | 3 |
| `engine/reflection/RegisterCoreTypes.h` (modify) | declare `registerLogicGraphComponent` | 3 |
| `engine/scene/SceneFormat.h` (modify) | `SceneEntity.components`; drop the 4 optional/string fields | 4 |
| `engine/scene/SceneIO.h`/`.cpp` (modify) | thread `ComponentRegistry&`; generic component read/write + back-compat shim | 4 |
| `tests/test_scene_io.cpp` (modify) | generic round-trip + back-compat + brand-new-type test | 4 |
| `engine/editor/SceneInspector.h`/`.cpp` (modify) | generic registry-driven Add/edit/remove | 5 |
| `games/11-sandbox/main.cpp` (modify) | registry wiring; typed bag reads in Play/Stop + node-editor assign/load; updated SceneIO calls | 6 |
| `engine/CMakeLists.txt`, `tests/CMakeLists.txt` (modify) | new sources + tests | 1–4 |

---

## Task 1: `ComponentSet` — type-erased deep-copyable bag

**Files:**
- Create: `engine/world/ComponentSet.h`
- Test: `tests/test_component_set.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_component_set.cpp`:

```cpp
#include "world/ComponentSet.h"
#include "test_framework.h"

using namespace iron;

namespace {
struct A { int x = 0; };
struct B { float y = 0.0f; std::string s; };
}  // namespace

int main() {
    // add / get / has / remove
    {
        ComponentSet cs;
        CHECK(!cs.has<A>());
        A* a = cs.add<A>(A{7});
        CHECK(a != nullptr);
        CHECK(cs.has<A>());
        CHECK(cs.get<A>() != nullptr);
        CHECK(cs.get<A>()->x == 7);
        CHECK(cs.get<B>() == nullptr);          // absent type → null

        // add-or-replace: adding A again replaces it
        cs.add<A>(A{9});
        CHECK(cs.get<A>()->x == 9);

        cs.add<B>(B{1.5f, "hi"});
        CHECK(cs.has<B>());
        CHECK(cs.get<B>()->s == "hi");

        cs.remove<A>();
        CHECK(!cs.has<A>());
        CHECK(cs.has<B>());                     // unrelated type untouched
    }

    // deep copy independence (matters for Play/Stop scene snapshots)
    {
        ComponentSet original;
        original.add<A>(A{1});
        ComponentSet copy = original;           // copy ctor → clone each box
        copy.get<A>()->x = 42;
        CHECK(original.get<A>()->x == 1);        // original unaffected
        CHECK(copy.get<A>()->x == 42);

        ComponentSet assigned;
        assigned = original;                     // copy assignment
        assigned.get<A>()->x = 99;
        CHECK(original.get<A>()->x == 1);
    }

    // generic iteration: all() exposes typeId + data()
    {
        ComponentSet cs;
        cs.add<A>(A{3});
        cs.add<B>(B{2.0f, "z"});
        int count = 0;
        bool sawA = false, sawB = false;
        for (const auto& box : cs.all()) {
            ++count;
            if (box->typeId() == componentTypeId<A>()) {
                sawA = true;
                CHECK(static_cast<const A*>(box->data())->x == 3);
            }
            if (box->typeId() == componentTypeId<B>()) sawB = true;
        }
        CHECK(count == 2);
        CHECK(sawA && sawB);
        CHECK(cs.hasTypeId(componentTypeId<A>()));
        cs.removeTypeId(componentTypeId<A>());
        CHECK(!cs.has<A>());
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test, run it, verify it fails**

In `tests/CMakeLists.txt`, after `iron_add_test(test_reflection test_reflection.cpp)` (or near the other world/reflection tests), add:
```cmake
iron_add_test(test_component_set test_component_set.cpp)
```
Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_component_set`
Expected: FAIL — `world/ComponentSet.h` not found.

- [ ] **Step 3: Implement `ComponentSet.h`**

Create `engine/world/ComponentSet.h`:

```cpp
#pragma once

#include "world/Entity.h"   // componentTypeId<T>()

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace iron {

// Type-erased, value-semantic holder for one component. clone() gives deep copy
// without needing a registry at copy time (mirrors World::IComponentArray).
struct IComponentBox {
    virtual ~IComponentBox() = default;
    virtual std::uint32_t typeId() const = 0;
    virtual std::unique_ptr<IComponentBox> clone() const = 0;
    virtual void*       data()       = 0;
    virtual const void* data() const = 0;
};

template <class T>
struct ComponentBox : IComponentBox {
    T value;
    ComponentBox() = default;
    explicit ComponentBox(const T& v) : value(v) {}
    std::uint32_t typeId() const override { return componentTypeId<T>(); }
    std::unique_ptr<IComponentBox> clone() const override {
        return std::make_unique<ComponentBox<T>>(value);
    }
    void*       data()       override { return &value; }
    const void* data() const override { return &value; }
};

// A per-entity bag of at most one component per type. Deep-copyable.
class ComponentSet {
public:
    ComponentSet() = default;
    ComponentSet(const ComponentSet& o) { copyFrom(o); }
    ComponentSet& operator=(const ComponentSet& o) {
        if (this != &o) copyFrom(o);
        return *this;
    }
    ComponentSet(ComponentSet&&) = default;
    ComponentSet& operator=(ComponentSet&&) = default;

    template <class T> T* add(const T& v = {}) {       // add-or-replace
        removeTypeId(componentTypeId<T>());
        comps_.push_back(std::make_unique<ComponentBox<T>>(v));
        return &static_cast<ComponentBox<T>*>(comps_.back().get())->value;
    }
    template <class T> T* get() {
        for (auto& b : comps_)
            if (b->typeId() == componentTypeId<T>())
                return static_cast<T*>(b->data());
        return nullptr;
    }
    template <class T> const T* get() const {
        return const_cast<ComponentSet*>(this)->get<T>();
    }
    template <class T> bool has() const { return hasTypeId(componentTypeId<T>()); }
    template <class T> void remove() { removeTypeId(componentTypeId<T>()); }

    bool hasTypeId(std::uint32_t id) const {
        for (auto& b : comps_) if (b->typeId() == id) return true;
        return false;
    }
    void removeTypeId(std::uint32_t id) {
        for (std::size_t i = 0; i < comps_.size(); ++i)
            if (comps_[i]->typeId() == id) { comps_.erase(comps_.begin() + i); return; }
    }
    void addBox(std::unique_ptr<IComponentBox> box) {
        if (!box) return;
        removeTypeId(box->typeId());
        comps_.push_back(std::move(box));
    }
    std::span<const std::unique_ptr<IComponentBox>> all() const { return comps_; }

private:
    void copyFrom(const ComponentSet& o) {
        comps_.clear();
        comps_.reserve(o.comps_.size());
        for (const auto& b : o.comps_) comps_.push_back(b->clone());
    }
    std::vector<std::unique_ptr<IComponentBox>> comps_;
};

}  // namespace iron
```

- [ ] **Step 4: Run the test, verify it passes**

Run: `cmake --build build-vk --target test_component_set` then `ctest --test-dir build-vk -C Debug -R test_component_set --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 5: Commit**

```bash
git add engine/world/ComponentSet.h tests/test_component_set.cpp tests/CMakeLists.txt
git commit -m "M67: ComponentSet — type-erased deep-copyable per-entity component bag

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `ComponentRegistry`

**Files:**
- Create: `engine/world/ComponentRegistry.h`, `engine/world/ComponentRegistry.cpp`
- Test: `tests/test_component_registry.cpp`
- Modify: `engine/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_component_registry.cpp`:

```cpp
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "reflection/Reflection.h"
#include "test_framework.h"

using namespace iron;

namespace {
struct Widget { int n = 0; float f = 0.0f; };
}  // namespace

int main() {
    Reflection r;
    r.registerType<Widget>("Widget")
        .field("n", &Widget::n)
        .field("f", &Widget::f);

    ComponentRegistry reg;
    reg.registerComponent<Widget>("Widget", r);

    // lookup by typeId and by name resolve to the same entry
    const ComponentRegistry::Entry* byId   = reg.byTypeId(componentTypeId<Widget>());
    const ComponentRegistry::Entry* byName = reg.byName("Widget");
    CHECK(byId != nullptr);
    CHECK(byName != nullptr);
    CHECK(byId == byName);
    CHECK(byId->name == "Widget");
    CHECK(byId->fields.size() == 2u);              // captured from Reflection

    // order() lists the registered type
    CHECK(reg.order().size() == 1u);
    CHECK(reg.order()[0] == componentTypeId<Widget>());

    // factory produces a box of the right type that ComponentSet accepts
    auto box = byId->factory();
    CHECK(box != nullptr);
    CHECK(box->typeId() == componentTypeId<Widget>());
    static_cast<Widget*>(box->data())->n = 5;
    ComponentSet cs;
    cs.addBox(std::move(box));
    CHECK(cs.get<Widget>() != nullptr);
    CHECK(cs.get<Widget>()->n == 5);

    // unknown lookups → null
    CHECK(reg.byName("Nope") == nullptr);
    CHECK(reg.byTypeId(250) == nullptr);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test, run it, verify it fails**

In `tests/CMakeLists.txt`, after the `test_component_set` line add:
```cmake
iron_add_test(test_component_registry test_component_registry.cpp)
```
Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_component_registry`
Expected: FAIL — `world/ComponentRegistry.h` not found.

- [ ] **Step 3: Implement the registry header**

Create `engine/world/ComponentRegistry.h`:

```cpp
#pragma once

#include "reflection/FieldDesc.h"
#include "reflection/Reflection.h"
#include "world/ComponentSet.h"
#include "world/Entity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

// Registry of authorable component TYPES, layered on Reflection. Register a type
// once (it must already be Reflection::registerType<T>()'d) and it becomes
// available to the Inspector's Add-Component menu and to SceneIO generically.
class ComponentRegistry {
public:
    struct Entry {
        std::uint32_t              typeId = 0;
        std::string_view           name;
        std::span<const FieldDesc> fields;   // lives in the Reflection instance
        std::function<std::unique_ptr<IComponentBox>()> factory;
    };

    template <class T>
    void registerComponent(std::string_view name, const Reflection& r) {
        Entry e;
        e.typeId  = componentTypeId<T>();
        e.name    = name;
        e.fields  = r.fieldsOf<T>();
        e.factory = [] { return std::unique_ptr<IComponentBox>(
                             std::make_unique<ComponentBox<T>>()); };
        if (entries_.find(e.typeId) == entries_.end()) order_.push_back(e.typeId);
        entries_[e.typeId] = std::move(e);
    }

    const std::vector<std::uint32_t>& order() const { return order_; }

    const Entry* byTypeId(std::uint32_t id) const {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }
    const Entry* byName(std::string_view name) const {
        for (const auto& [id, e] : entries_) if (e.name == name) return &e;
        return nullptr;
    }

private:
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::vector<std::uint32_t> order_;
};

}  // namespace iron
```

Create `engine/world/ComponentRegistry.cpp` (keeps a TU for the target even though the class is header-only today):

```cpp
#include "world/ComponentRegistry.h"

// ComponentRegistry is currently header-only (all methods are templates or
// inline). This TU exists so the build has a stable object file to attach the
// symbol to and to host non-inline additions later.
namespace iron {}  // namespace iron
```

- [ ] **Step 4: Add the .cpp to the engine build**

In `engine/CMakeLists.txt`, add `world/ComponentRegistry.cpp` to the `ironcore` source list (find where `world/World.*`/`world/CollisionShape.reflect.cpp` etc. are listed and add it alongside).

- [ ] **Step 5: Run the test, verify it passes**

Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_component_registry` then `ctest --test-dir build-vk -C Debug -R test_component_registry --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 6: Commit**

```bash
git add engine/world/ComponentRegistry.h engine/world/ComponentRegistry.cpp engine/CMakeLists.txt tests/test_component_registry.cpp tests/CMakeLists.txt
git commit -m "M67: ComponentRegistry — authorable component types on top of Reflection

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `LogicGraphComponent` + core-component registration

**Files:**
- Create: `engine/gameplay/LogicGraphComponent.h`, `engine/gameplay/LogicGraphComponent.reflect.cpp`
- Create: `engine/scene/RegisterCoreComponents.h`, `engine/scene/RegisterCoreComponents.cpp`
- Modify: `engine/reflection/RegisterCoreTypes.h`, `engine/CMakeLists.txt`, `tests/test_component_registry.cpp` (extend), `tests/CMakeLists.txt`

Additive only — no existing struct changes yet.

- [ ] **Step 1: Write the failing test (extend registry test)**

Append a second `main`-body block to `tests/test_component_registry.cpp` is not possible (single main); instead add a NEW test file `tests/test_core_components.cpp`:

```cpp
#include "scene/RegisterCoreComponents.h"
#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
#include "world/ComponentRegistry.h"
#include "world/CollisionShape.h"
#include "audio/AudioEmitter.h"
#include "render/ReflectionProbe.h"
#include "gameplay/LogicGraphComponent.h"
#include "test_framework.h"

using namespace iron;

int main() {
    Reflection r;
    registerCollisionShape(r);
    registerAudioEmitter(r);
    registerReflectionProbe(r);
    registerLogicGraphComponent(r);

    ComponentRegistry reg;
    registerCoreComponents(reg, r);

    CHECK(reg.byName("CollisionShape")     != nullptr);
    CHECK(reg.byName("AudioEmitter")       != nullptr);
    CHECK(reg.byName("ReflectionProbeDef") != nullptr);
    CHECK(reg.byName("LogicGraphComponent")!= nullptr);
    CHECK(reg.byTypeId(componentTypeId<CollisionShape>()) != nullptr);
    CHECK(reg.order().size() == 4u);

    // LogicGraphComponent has its one reflected string field.
    CHECK(reg.byName("LogicGraphComponent")->fields.size() == 1u);

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test, run it, verify it fails**

In `tests/CMakeLists.txt` add:
```cmake
iron_add_test(test_core_components test_core_components.cpp)
```
Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_core_components`
Expected: FAIL — `gameplay/LogicGraphComponent.h` / `scene/RegisterCoreComponents.h` not found.

- [ ] **Step 3: Create `LogicGraphComponent` + its reflection**

Create `engine/gameplay/LogicGraphComponent.h`:

```cpp
#pragma once

#include <string>

namespace iron {

// Authorable wrapper for an entity's logic node-graph. `graph` is the serialized
// node graph (nlohmann::json::dump of the Graph) — same payload that used to live
// in SceneEntity::logicGraph. Empty = no graph.
struct LogicGraphComponent {
    std::string graph;
};

}  // namespace iron
```

Create `engine/gameplay/LogicGraphComponent.reflect.cpp`:

```cpp
#include "gameplay/LogicGraphComponent.h"
#include "reflection/Reflection.h"

namespace iron {

void registerLogicGraphComponent(Reflection& r) {
    r.registerType<LogicGraphComponent>("LogicGraphComponent")
        .field("graph", &LogicGraphComponent::graph);
}

}  // namespace iron
```

- [ ] **Step 4: Declare the new reflect fn**

In `engine/reflection/RegisterCoreTypes.h`, add to the declaration list:
```cpp
void registerLogicGraphComponent(Reflection& r);
```

- [ ] **Step 5: Create the core-component registration helper**

Create `engine/scene/RegisterCoreComponents.h`:

```cpp
#pragma once

namespace iron {
class Reflection;
class ComponentRegistry;

// Register the engine's authorable components into `cr`. The corresponding
// Reflection types must already be registered in `r` (registerCollisionShape,
// registerAudioEmitter, registerReflectionProbe, registerLogicGraphComponent).
void registerCoreComponents(ComponentRegistry& cr, const Reflection& r);
}  // namespace iron
```

Create `engine/scene/RegisterCoreComponents.cpp`:

```cpp
#include "scene/RegisterCoreComponents.h"

#include "audio/AudioEmitter.h"
#include "gameplay/LogicGraphComponent.h"
#include "render/ReflectionProbe.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"

namespace iron {

void registerCoreComponents(ComponentRegistry& cr, const Reflection& r) {
    cr.registerComponent<CollisionShape>("CollisionShape", r);
    cr.registerComponent<AudioEmitter>("AudioEmitter", r);
    cr.registerComponent<ReflectionProbeDef>("ReflectionProbeDef", r);
    cr.registerComponent<LogicGraphComponent>("LogicGraphComponent", r);
}

}  // namespace iron
```

- [ ] **Step 6: Add new .cpp files to the engine build**

In `engine/CMakeLists.txt`, add to the `ironcore` sources:
```
gameplay/LogicGraphComponent.reflect.cpp
scene/RegisterCoreComponents.cpp
```
(Place near the existing `*.reflect.cpp` and `scene/*` entries.)

- [ ] **Step 7: Run the test, verify it passes**

Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_core_components` then `ctest --test-dir build-vk -C Debug -R test_core_components --output-on-failure`
Expected: PASS (1/1).

- [ ] **Step 8: Commit**

```bash
git add engine/gameplay/LogicGraphComponent.h engine/gameplay/LogicGraphComponent.reflect.cpp engine/scene/RegisterCoreComponents.h engine/scene/RegisterCoreComponents.cpp engine/reflection/RegisterCoreTypes.h engine/CMakeLists.txt tests/test_core_components.cpp tests/CMakeLists.txt
git commit -m "M67: LogicGraphComponent + registerCoreComponents helper + test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Migrate `SceneEntity` to `ComponentSet` + generic SceneIO

**Files:**
- Modify: `engine/scene/SceneFormat.h`, `engine/scene/SceneIO.h`, `engine/scene/SceneIO.cpp`, `tests/test_scene_io.cpp`

**Build scope for this task:** build/run only `ironcore` + the test targets (`test_scene_io`, `test_component_*`, `test_core_components`). `ironcore_editor` and `sandbox` will NOT compile until Tasks 5–6 — that is expected; do not build them here.

- [ ] **Step 1: Update the SceneIO round-trip test (failing)**

In `tests/test_scene_io.cpp`: (a) update `makeReflectionRegistry()` to also call `registerLogicGraphComponent(r)`; (b) build a `ComponentRegistry` helper; (c) replace the M42 optional-components test block (lines ~193–242) with the generic version. Add at top:
```cpp
#include "world/ComponentRegistry.h"
#include "scene/RegisterCoreComponents.h"
#include "gameplay/LogicGraphComponent.h"
```
Helper after `makeReflectionRegistry()`:
```cpp
static iron::ComponentRegistry makeComponentRegistry(const iron::Reflection& r) {
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    return cr;
}
```
Replace the optional-components test block with:
```cpp
    // M67: generic component round-trip via the registry.
    {
        iron::Reflection r = makeReflectionRegistry();
        iron::ComponentRegistry cr = makeComponentRegistry(r);

        iron::SceneFile scene;
        iron::SceneEntity e;
        e.name = "thing";
        e.components.add<iron::CollisionShape>(
            iron::CollisionShape{ iron::ColliderShape::Sphere, iron::ColliderBody::Dynamic,
                                  {1,1,1}, 2.0f, 0.5f, 3.0f });
        e.components.add<iron::AudioEmitter>(iron::AudioEmitter{ "boom.wav", 0.8f, true, true, false });
        e.components.add<iron::LogicGraphComponent>(iron::LogicGraphComponent{ "{\"nodes\":[]}" });
        scene.entities.push_back(e);

        const std::string json = iron::sceneToJsonString(r, cr, scene);
        const iron::SceneFile back = iron::sceneFromJsonString(r, cr, json);

        CHECK(back.entities.size() == 1u);
        const iron::SceneEntity& b = back.entities[0];
        CHECK(b.components.has<iron::CollisionShape>());
        CHECK(b.components.get<iron::CollisionShape>()->shape == iron::ColliderShape::Sphere);
        CHECK(b.components.get<iron::CollisionShape>()->mass == 3.0f);
        CHECK(b.components.has<iron::AudioEmitter>());
        CHECK(b.components.get<iron::AudioEmitter>()->wavPath == "boom.wav");
        CHECK(b.components.has<iron::LogicGraphComponent>());
        CHECK(b.components.get<iron::LogicGraphComponent>()->graph == "{\"nodes\":[]}");
        CHECK(!b.components.has<iron::ReflectionProbeDef>());   // absent stays absent
    }

    // M67: legacy back-compat — old top-level keys still load.
    {
        iron::Reflection r = makeReflectionRegistry();
        iron::ComponentRegistry cr = makeComponentRegistry(r);
        const std::string legacy = R"({"entities":[{"name":"old",
            "collision":{"shape":"box","body":"static","halfExtents":[2,2,2]},
            "logicGraph":"{\"v\":1}"}]})";
        const iron::SceneFile back = iron::sceneFromJsonString(r, cr, legacy);
        CHECK(back.entities.size() == 1u);
        CHECK(back.entities[0].components.has<iron::CollisionShape>());
        CHECK(back.entities[0].components.get<iron::CollisionShape>()->shape == iron::ColliderShape::Box);
        CHECK(back.entities[0].components.has<iron::LogicGraphComponent>());
        CHECK(back.entities[0].components.get<iron::LogicGraphComponent>()->graph == "{\"v\":1}");
    }

    // M67 headline: a brand-new component type round-trips with NO SceneIO edits.
    {
        struct DummyComp { int n = 0; float f = 0.0f; };
        iron::Reflection r = makeReflectionRegistry();
        r.registerType<DummyComp>("DummyComp").field("n", &DummyComp::n).field("f", &DummyComp::f);
        iron::ComponentRegistry cr = makeComponentRegistry(r);
        cr.registerComponent<DummyComp>("DummyComp", r);   // one call — that's the whole point

        iron::SceneFile scene;
        iron::SceneEntity e; e.name = "x";
        e.components.add<DummyComp>(DummyComp{ 11, 2.5f });
        scene.entities.push_back(e);

        const iron::SceneFile back =
            iron::sceneFromJsonString(r, cr, iron::sceneToJsonString(r, cr, scene));
        CHECK(back.entities[0].components.get<DummyComp>() != nullptr);
        CHECK(back.entities[0].components.get<DummyComp>()->n == 11);
        CHECK(back.entities[0].components.get<DummyComp>()->f == 2.5f);
    }
```
Also fix any OTHER references in this test file to `e.collision`/`e.audio`/`e.probe`/`e.logicGraph` (e.g. earlier blocks) to use `e.components`. Search the file for `.collision`, `.audio`, `.probe`, `.logicGraph` and convert.

- [ ] **Step 2: Run the test, verify it fails to compile**

Run: `cmake --build build-vk --target test_scene_io`
Expected: FAIL — `SceneEntity` has no member `components`; `sceneToJsonString`/`sceneFromJsonString` don't take a `ComponentRegistry`.

- [ ] **Step 3: Change `SceneEntity`**

In `engine/scene/SceneFormat.h`: add `#include "world/ComponentSet.h"`, remove the includes only needed by the dropped optionals if they're now unused (keep `audio/AudioEmitter.h`, `render/ReflectionProbe.h`, `world/CollisionShape.h` includes — they're still used by callers via the registry, but SceneFormat itself no longer needs them; leave them to avoid churn). Replace the four fields:
```cpp
struct SceneEntity {
    std::string  name;
    Transform    transform;
    MeshRef      mesh;
    MaterialDef  material;
    ComponentSet components;   // M67 — collision/audio/probe/logic are now components
};
```

- [ ] **Step 4: Thread `ComponentRegistry` through SceneIO signatures**

In `engine/scene/SceneIO.h`, add `class ComponentRegistry;` forward decl and add a `const ComponentRegistry& cr` parameter (after `const Reflection& r`) to: `loadSceneFile`, `saveSceneFile`, `sceneToJsonString`, `sceneFromJsonString`. Example:
```cpp
std::string sceneToJsonString(const Reflection& r, const ComponentRegistry& cr, const SceneFile& scene);
SceneFile   sceneFromJsonString(const Reflection& r, const ComponentRegistry& cr, const std::string& json);
bool        saveSceneFile(const Reflection& r, const ComponentRegistry& cr, const SceneFile& scene, const std::string& path);
std::optional<SceneFile> loadSceneFile(const Reflection& r, const ComponentRegistry& cr, const std::string& path);
```
(Match the existing return types/names; only add the `cr` parameter.)

- [ ] **Step 5: Rewrite `entityToJson`/`entityFromJson` generically**

In `engine/scene/SceneIO.cpp`: add includes
```cpp
#include "world/ComponentRegistry.h"
#include "world/CollisionShape.h"
#include "audio/AudioEmitter.h"
#include "render/ReflectionProbe.h"
#include "gameplay/LogicGraphComponent.h"
```
Give `entityToJson`/`entityFromJson` (and the internal callers) the `const ComponentRegistry& cr` parameter, and replace their bodies:
```cpp
json entityToJson(const Reflection& r, const ComponentRegistry& cr, const SceneEntity& e) {
    json j = json::object();
    j["name"]      = e.name;
    j["transform"] = componentToJson(r, e.transform);
    j["mesh"]      = componentToJson(r, e.mesh);
    j["material"]  = componentToJson(r, e.material);
    json comps = json::array();
    for (const auto& box : e.components.all()) {
        const ComponentRegistry::Entry* entry = cr.byTypeId(box->typeId());
        if (!entry) continue;   // unregistered → skip (shouldn't happen)
        json cj = componentToJsonByPtr(r, entry->fields, box->data());
        cj["type"] = std::string(entry->name);
        comps.push_back(std::move(cj));
    }
    if (!comps.empty()) j["components"] = std::move(comps);
    return j;
}

SceneEntity entityFromJson(const Reflection& r, const ComponentRegistry& cr, const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    if (j.contains("transform")) componentFromJson(r, e.transform, j["transform"]);
    if (j.contains("mesh"))      componentFromJson(r, e.mesh,      j["mesh"]);
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);

    // New generic format.
    if (j.contains("components") && j["components"].is_array()) {
        for (const json& cj : j["components"]) {
            if (!cj.contains("type") || !cj["type"].is_string()) continue;
            const ComponentRegistry::Entry* entry = cr.byName(cj["type"].get<std::string>());
            if (!entry) continue;                 // unknown type → skip (forward-compat)
            auto box = entry->factory();
            componentFromJsonByPtr(r, entry->fields, box->data(), cj);
            e.components.addBox(std::move(box));
        }
    }

    // Back-compat: legacy top-level keys → components (writing always uses the new format).
    auto legacy = [&](const char* key, std::string_view typeName) {
        if (!j.contains(key)) return;
        const ComponentRegistry::Entry* entry = cr.byName(typeName);
        if (!entry) return;
        auto box = entry->factory();
        componentFromJsonByPtr(r, entry->fields, box->data(), j[key]);
        e.components.addBox(std::move(box));
    };
    legacy("collision", "CollisionShape");
    legacy("audio",     "AudioEmitter");
    legacy("probe",     "ReflectionProbeDef");
    if (j.contains("logicGraph") && j["logicGraph"].is_string()) {
        if (const auto* entry = cr.byName("LogicGraphComponent")) {
            auto box = entry->factory();
            static_cast<LogicGraphComponent*>(box->data())->graph =
                j["logicGraph"].get<std::string>();
            e.components.addBox(std::move(box));
        }
    }
    return e;
}
```
Update the internal call sites inside `sceneToJsonString`/`sceneFromJsonString`/`saveSceneFile`/`loadSceneFile` to pass `cr` to `entityToJson`/`entityFromJson`.

- [ ] **Step 6: Update the test's other call sites + run**

Make sure all `sceneToJsonString`/`sceneFromJsonString`/`saveSceneFile`/`loadSceneFile` calls in `tests/test_scene_io.cpp` pass the new `cr` argument (build the registry via `makeComponentRegistry(r)` in each block).
Run: `cmake --build build-vk --target test_scene_io` then `ctest --test-dir build-vk -C Debug -R "test_scene_io|test_component_set|test_component_registry|test_core_components" --output-on-failure`
Expected: all PASS. (Do NOT build ironcore_editor/sandbox yet.)

- [ ] **Step 7: Commit**

```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp
git commit -m "M67: SceneEntity uses ComponentSet; generic registry-driven SceneIO + back-compat

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Generic Inspector

**Files:**
- Modify: `engine/editor/SceneInspector.h`, `engine/editor/SceneInspector.cpp`

Depends on Task 4. Restores `ironcore_editor`.

- [ ] **Step 1: Add `ComponentRegistry&` to the Inspector signature**

In `engine/editor/SceneInspector.h`: forward-declare `class ComponentRegistry;` and add it to `draw`:
```cpp
bool draw(const Reflection& reflection,
          const ComponentRegistry& registry,
          SceneEntity* entity,
          GizmoSpace& space,
          EffectKind& effectKind);
```

- [ ] **Step 2: Replace the hardcoded `kOptional` table with registry iteration**

In `engine/editor/SceneInspector.cpp`: remove the `OptionalComp`/`kOptional` block (lines ~59–82) and the loop/combo that used it (lines ~84–99). First, factor the existing reflected-component renderer into a by-pointer worker so it can be called generically. If the current `renderComponent<T>(r, obj)` template wraps a per-field dispatch, expose its body as:
```cpp
// near the existing renderComponent template, add a non-template worker:
bool renderComponentByPtr(const Reflection& r, std::span<const FieldDesc> fields, void* obj);
```
(Move the field-iteration body of the existing `renderComponent` into `renderComponentByPtr`; have the template call it: `return renderComponentByPtr(r, r.fieldsOf<T>(), &obj);`.)

Then render components generically:
```cpp
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
// ...
// Existing components on the entity:
for (const auto& box : e.components.all()) {
    const ComponentRegistry::Entry* entry = registry.byTypeId(box->typeId());
    if (!entry) continue;
    ImGui::PushID(static_cast<int>(entry->typeId));
    ImGui::SeparatorText(std::string(entry->name).c_str());
    changed |= renderComponentByPtr(reflection, entry->fields,
                                    const_cast<void*>(box->data()));
    if (ImGui::SmallButton("Remove")) { e.components.removeTypeId(entry->typeId); changed = true; }
    ImGui::PopID();
}
// Add Component menu: every registered type the entity lacks.
if (ImGui::BeginCombo("Add Component", "Add Component ...")) {
    for (std::uint32_t id : registry.order()) {
        if (e.components.hasTypeId(id)) continue;
        const ComponentRegistry::Entry* entry = registry.byTypeId(id);
        if (entry && ImGui::Selectable(std::string(entry->name).c_str())) {
            e.components.addBox(entry->factory());
            changed = true;
        }
    }
    ImGui::EndCombo();
}
```
(`box->data()` is `const void*` for a const range; the set is mutable here so use the non-const `all()` element — if `all()` returns const, add a small non-const accessor or cast as shown. Prefer adding `ComponentSet::all()` non-const overload returning `std::span<std::unique_ptr<IComponentBox>>` so the cast isn't needed.)

- [ ] **Step 3: Build the editor lib**

Run: `cmake --build build-vk --target ironcore_editor`
Expected: EXIT CODE 0. (Sandbox still won't link — next task.)

- [ ] **Step 4: Commit**

```bash
git add engine/editor/SceneInspector.h engine/editor/SceneInspector.cpp engine/world/ComponentSet.h
git commit -m "M67: Inspector drives Add/edit/remove from the ComponentRegistry

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Sandbox host wiring

**Files:**
- Modify: `games/11-sandbox/main.cpp`

Depends on Tasks 4–5. Restores the full build.

- [ ] **Step 1: Create + populate a `ComponentRegistry` next to the Reflection**

After the reflection registration block (`games/11-sandbox/main.cpp:164-171`), add `registerLogicGraphComponent(reflection);` and then:
```cpp
iron::ComponentRegistry componentRegistry;
iron::registerCoreComponents(componentRegistry, reflection);
```
Add includes near the top: `#include "world/ComponentRegistry.h"`, `#include "scene/RegisterCoreComponents.h"`, `#include "gameplay/LogicGraphComponent.h"`.

- [ ] **Step 2: Update all SceneIO calls + the Inspector call**

Pass `componentRegistry` to every `saveSceneFile`/`loadSceneFile`/`sceneToJsonString`/`sceneFromJsonString` call (including the M57 undo snapshot save/restore), and to `inspector.draw(reflection, componentRegistry, entity, space, effectKind)`.

- [ ] **Step 3: Convert Play/Stop + node-editor reads to the bag (typed)**

Apply these exact conversions (line refs from the current file):
- Collision read (`:679`): `if (e.collision)` → `if (const auto* csp = e.components.get<iron::CollisionShape>())`, then use `*csp` (rename the local `cs` to bind to `*csp`).
- Audio read (`:701`): `if (e.audio && e.audio->playOnStart)` → `const auto* emp = e.components.get<iron::AudioEmitter>(); if (emp && emp->playOnStart)`, use `*emp`.
- Probe viz (`:988`): `if (!e.probe) return;` → `const auto* pp = e.components.get<iron::ReflectionProbeDef>(); if (!pp) return;` and replace `e.probe->halfExtents` with `pp->halfExtents`.
- LogicGraph read (`:669`): replace `if (!e.logicGraph.empty() && ...)` with
  ```cpp
  const auto* lgc = e.components.get<iron::LogicGraphComponent>();
  if (lgc && !lgc->graph.empty() && i < (int)sceneIndexToEntity.size()) {
      auto parsed = nlohmann::json::parse(lgc->graph, nullptr, false);
      ... (rest unchanged, using lgc->graph)
  }
  ```
- Collision write-back on Stop (`:1109`): `scene.entities[idx].collision && scene.entities[idx].collision->body == Dynamic` →
  ```cpp
  auto* csb = scene.entities[idx].components.get<iron::CollisionShape>();
  if (csb && csb->body == iron::ColliderBody::Dynamic) { ... }
  ```
- LogicGraph assign (`:1409`): `scene.entities[selectedIndex].logicGraph = graphModel.toJson().dump();` →
  ```cpp
  scene.entities[selectedIndex].components
      .add<iron::LogicGraphComponent>(iron::LogicGraphComponent{ graphModel.toJson().dump() });
  ```
- LogicGraph load-from-entity (`:1412`): read from the component:
  ```cpp
  const auto* lgc = scene.entities[selectedIndex].components.get<iron::LogicGraphComponent>();
  auto parsed = nlohmann::json::parse(lgc ? lgc->graph : std::string{}, nullptr, false);
  ```

- [ ] **Step 4: Build the sandbox**

Run: `cmake --build build-vk --target sandbox` (kill any running `sandbox.exe` first if LNK1168).
Expected: EXIT CODE 0.

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M67: sandbox host wires ComponentRegistry + typed bag reads in Play/Stop

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Full build, tests, visual gate, PR

- [ ] **Step 1: Full build of ALL targets**

Run: `cmake --build build-vk` — confirm `$LASTEXITCODE` 0 and `rg -n "error|LNK|fatal" <log>` shows nothing.

- [ ] **Step 2: Full test suite**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all pass (prior 80 + test_component_set + test_component_registry + test_core_components = 83; test_scene_io still one test). Confirm `100% tests passed`.

- [ ] **Step 3: Visual gate (editor)**

Launch the sandbox editor (`build-vk/games/11-sandbox/Debug/sandbox.exe`). Verify:
1. Select an entity → Inspector shows core (transform/mesh/material) + an **"Add Component"** dropdown listing CollisionShape / AudioEmitter / ReflectionProbeDef / LogicGraphComponent.
2. Add CollisionShape → its fields edit; **Remove** removes it. Add AudioEmitter similarly.
3. Save the scene, reload → components persist (open the saved JSON; confirm the `"components"` array).
4. Assign a logic graph in the Node Editor → it round-trips (Play shows the graph driving the entity, as before M67).
5. Enter Play with a dynamic CollisionShape → physics still works; Stop restores transforms.

- [ ] **Step 4: Push + PR (after the user confirms the gate)**

```bash
git push -u origin m67-extensible-component-model
gh pr create --base main --title "M67: Extensible component model (registry-driven components)" --body "$(cat <<'EOF'
## M67 — Extensible Component Model

Replaces SceneEntity's hardcoded std::optional<X> components with a type-erased, registry-driven ComponentSet built on the existing Reflection. Adding an authorable component is now one registration call.

- `ComponentSet` — type-erased, deep-copyable per-entity bag (unit-tested incl. deep-copy independence).
- `ComponentRegistry` — authorable component types on top of Reflection (name + fields + factory).
- `LogicGraphComponent` wraps the old logicGraph string; collision/audio/probe/logic all migrated to the registry.
- Generic Inspector (Add-Component menu over the registry) + generic SceneIO (`"components"` array) with a legacy back-compat read shim.
- First milestone of the Component + Hierarchy overhaul (M67 spine -> M68 systems-as-components -> M69 hierarchy).

Headline test: a brand-new component type round-trips through SceneIO with zero IO/Inspector edits. Tests: 83/83. Editor visual gate: Add/edit/remove/persist/Play all verified.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 5: Watch CI; squash-merge when green; update memory**

`gh pr checks <N> --watch --interval 30`; when green `gh pr merge <N> --squash --delete-branch`; sync main; update MEMORY.md (M67 done; M68 next = systems-as-components).

---

## Self-Review Notes (author)

- **Spec coverage:** ComponentSet (T1), ComponentRegistry (T2), LogicGraphComponent + core registration (T3), SceneEntity swap + generic SceneIO + back-compat (T4), Inspector (T5), Play/Stop/node-editor wiring (T6), tests incl. the headline new-type test (T4), visual gate (T7). All spec sections covered.
- **Type consistency:** `ComponentSet::add/get/has/remove/all/hasTypeId/removeTypeId/addBox`, `IComponentBox::{typeId,clone,data}`, `ComponentRegistry::{registerComponent,Entry,order,byTypeId,byName}`, `registerCoreComponents(cr,r)`, `registerLogicGraphComponent(r)`, `LogicGraphComponent::graph` — all used consistently across tasks. SceneIO gains `const ComponentRegistry&` everywhere uniformly.
- **Known staged breakage:** Tasks 4–6 leave the tree partially non-building (editor/sandbox) until Task 6; each task states which targets it builds, and Task 7 confirms the full build. Acceptable for subagent-driven execution; do not PR before Task 7.
- **`renderComponentByPtr` / `ComponentSet::all()` non-const:** Task 5 factors the existing per-field renderer into a by-ptr worker and (preferably) adds a non-const `all()` overload to avoid a const_cast — flagged inline.
