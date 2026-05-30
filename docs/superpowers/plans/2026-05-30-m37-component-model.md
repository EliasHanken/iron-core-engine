# M37 — Component model: World + first three POD components Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `iron::World` (entity-component runtime container) and port the existing renderable entity shape into components: `Transform`, `MeshRef`, `MaterialDef`, `RenderHandles`. Flip the sandbox render submit to iterate the World. Sandbox loader decomposes `SceneEntity` on load; an end-of-frame sync mirrors editor edits into the World until M39 migrates the editor itself.

**Architecture:** Per-type dense `vector<T>` + sparse `vector<uint32_t>` index storage (`ComponentArray<T>`), wrapped by a type-erased `World` keyed by a compile-time `componentTypeId<T>()`. Generational `EntityId` (32-bit index + 32-bit generation) catches dangling references. Single-threaded. No reflection, no scheduling, no OO-component layer in v1 — those land in M38–M40. Mirrors the Unreal/Unity rule "everything about an entity lives on the entity" by putting GPU asset handles into a `RenderHandles` component, not a parallel host-side cache.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`). No new external dependencies. Reference spec: `docs/superpowers/specs/2026-05-30-m37-component-model-design.md`.

**Verification model:** The whole World API is **pure logic** — no Vulkan, no GLFW. All correctness is unit-tested in `tests/test_world.cpp`. Sandbox integration (Phase D) is GUI-only and gated on visual verification (Phase E). Every task's mechanical gate: **builds clean** (`cmake --build build-vk --config Debug --target ironcore`) and the **existing tests stay green** (`ctest --test-dir build-vk -C Debug`).

**Build & test commands (used by every task):**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_world      # after Task 1 lands
cmake --build build-vk --config Debug --target sandbox          # for Phase D tasks
ctest --test-dir build-vk -C Debug --output-on-failure
```
(A benign "LF will be replaced by CRLF" warning on commit is expected on Windows. Pre-existing ImGui/GLFW `LNK4217` linker warnings are benign.)

**Branch:** already on `feat/m37-component-model` (the spec commit `1352a0b` sits on the branch tip). Every task in this plan commits to this branch.

---

## File Structure

**New (pure logic, unit-tested):**
- `engine/world/Entity.h` — `EntityId` POD + `kEntityNone` + `componentTypeId<T>()`.
- `engine/world/ComponentArray.h` — templated dense-vec + sparse-index storage per component type.
- `engine/world/World.h` / `.cpp` — entity lifecycle + type-erased component arrays + `add<T>` / `get<T>` / `remove<T>` / `view<T>`.
- `engine/world/Transform.h` — POD `Transform { position, rotation, scale }`.
- `engine/render/RenderHandles.h` — POD `RenderHandles { mesh, albedo, normal, specular }` (uint32 handles).
- `tests/test_world.cpp` — all World unit tests + render-submit pseudocode integration test.

**Modified:**
- `engine/CMakeLists.txt` — add `world/World.cpp` to the `ironcore` source list.
- `tests/CMakeLists.txt` — `iron_add_test(test_world test_world.cpp)`.
- `games/11-sandbox/main.cpp` — build the World on load, flip the submit loop, mirror add/duplicate/delete, end-of-frame Inspector→World sync.

**Untouched on purpose:** `engine/scene/SceneFormat.h` (`MeshRef` and `MaterialDef` are reused as components), `SceneIO.{h,cpp}`, the renderer backend (Vulkan), all editor panels (`SceneOutliner`, `SceneInspector`, `EnvironmentPanel`), `Gizmo`, picking, `demo.json`.

**Phases:**
- **A** — World scaffold: `EntityId`, `ComponentArray<T>`, test harness. Pure logic, TDD.
- **B** — `World` typed component API + iteration. Pure logic, TDD.
- **C** — POD component types (`Transform`, `RenderHandles`) + render-submit pseudocode integration test.
- **D** — Sandbox integration (loader, submit, add/delete, Inspector→World sync).
- **E** — Visual verification, full-suite green, PR + squash-merge.

---

## Phase A — World scaffold (pure logic, TDD)

Goal: get `EntityId`, `componentTypeId<T>()`, and `ComponentArray<T>` correct and tested. Nothing renders yet.

### Task A1: `EntityId` + `componentTypeId<T>()` + test harness

**Files:**
- Create: `engine/world/Entity.h`
- Create: `tests/test_world.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_world.cpp`:

```cpp
#include "world/Entity.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_entityid_default_is_invalid() {
    iron::EntityId e;
    CHECK(!e.valid());
    CHECK(e == iron::kEntityNone);
}

static void test_entityid_with_generation_is_valid() {
    iron::EntityId e{0, 1};
    CHECK(e.valid());
    CHECK(!(e == iron::kEntityNone));
}

static void test_component_type_ids_are_distinct() {
    const auto a = iron::componentTypeId<int>();
    const auto b = iron::componentTypeId<float>();
    const auto a2 = iron::componentTypeId<int>();
    CHECK(a != b);
    CHECK(a == a2);
}

int main() {
    test_entityid_default_is_invalid();
    test_entityid_with_generation_is_valid();
    test_component_type_ids_are_distinct();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
```

Add the test registration to `tests/CMakeLists.txt` (append after the last `iron_add_test` line):
```cmake
iron_add_test(test_world test_world.cpp)
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `world/Entity.h` does not exist.

- [ ] **Step 3: Implement `Entity.h`**

Create `engine/world/Entity.h`:

```cpp
#pragma once

#include <cstdint>

namespace iron {

// Generational entity handle. generation == 0 is the sentinel "no entity".
struct EntityId {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const EntityId&) const = default;
};

inline constexpr EntityId kEntityNone{};

// Compile-time small-integer per type via a counter-template pattern.
// Returns the same value for the same T across translation units within
// one binary (function-local static initialisation), distinct across types.
namespace detail {
inline uint32_t nextComponentTypeId() {
    static uint32_t next = 0;
    return next++;
}
}  // namespace detail

template <class T>
inline uint32_t componentTypeId() {
    static const uint32_t id = detail::nextComponentTypeId();
    return id;
}

}  // namespace iron
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: `All world tests passed.` + `1/1 Test #N: test_world ... Passed`.

- [ ] **Step 5: Commit**

```bash
git add engine/world/Entity.h tests/test_world.cpp tests/CMakeLists.txt
git commit -m "M37: EntityId + componentTypeId + test scaffold"
```

---

### Task A2: `ComponentArray<T>` — add / get / size

**Files:**
- Create: `engine/world/ComponentArray.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_world.cpp` BEFORE `int main()`:

```cpp
#include "world/ComponentArray.h"

static void test_component_array_add_and_get() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    int* p = arr.add(e, 42);
    CHECK(p != nullptr);
    CHECK(*p == 42);
    CHECK(arr.get(e) != nullptr);
    CHECK(*arr.get(e) == 42);
    CHECK(arr.size() == 1);
}

static void test_component_array_get_missing_returns_null() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    CHECK(arr.get(e) == nullptr);
}

static void test_component_array_add_two_entities() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr.size() == 2);
    CHECK(*arr.get(a) == 10);
    CHECK(*arr.get(b) == 20);
}
```

Register the new tests in `main()`:
```cpp
    test_component_array_add_and_get();
    test_component_array_get_missing_returns_null();
    test_component_array_add_two_entities();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `world/ComponentArray.h` does not exist.

- [ ] **Step 3: Implement `ComponentArray.h`**

Create `engine/world/ComponentArray.h`:

```cpp
#pragma once

#include "world/Entity.h"

#include <cstdint>
#include <vector>

namespace iron {

// Per-component-type storage: dense vector for cache-friendly iteration,
// sparse index from EntityId.index -> dense row. v1 covers add / get only;
// remove and iteration land in later tasks.
template <class T>
class ComponentArray {
public:
    static constexpr uint32_t kNoRow = UINT32_MAX;

    T* add(EntityId e, const T& value = {}) {
        if (e.index >= sparse_.size()) sparse_.resize(e.index + 1, kNoRow);
        // Overwrite if already present (defined behaviour for this v1).
        if (sparse_[e.index] != kNoRow) {
            dense_[sparse_[e.index]] = value;
            return &dense_[sparse_[e.index]];
        }
        sparse_[e.index] = static_cast<uint32_t>(dense_.size());
        dense_.push_back(value);
        denseEntities_.push_back(e);
        return &dense_.back();
    }

    T* get(EntityId e) {
        if (e.index >= sparse_.size()) return nullptr;
        const uint32_t row = sparse_[e.index];
        if (row == kNoRow) return nullptr;
        // Guard against generation mismatch (stale handle into recycled slot).
        if (!(denseEntities_[row] == e)) return nullptr;
        return &dense_[row];
    }

    const T* get(EntityId e) const {
        return const_cast<ComponentArray*>(this)->get(e);
    }

    size_t size() const { return dense_.size(); }

protected:
    std::vector<T>        dense_;
    std::vector<EntityId> denseEntities_;
    std::vector<uint32_t> sparse_;
};

}  // namespace iron
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all `test_world` tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/ComponentArray.h tests/test_world.cpp
git commit -m "M37: ComponentArray<T> — add/get/size + tests"
```

---

### Task A3: `ComponentArray<T>` — `remove` (swap-and-pop)

**Files:**
- Modify: `engine/world/ComponentArray.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append BEFORE `int main()`:

```cpp
static void test_component_array_remove_invalidates_get() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    arr.add(e, 7);
    arr.remove(e);
    CHECK(arr.get(e) == nullptr);
    CHECK(arr.size() == 0);
}

static void test_component_array_remove_swaps_correctly() {
    // Insert A,B,C; remove B; verify C's sparse index points at row 1 (B's old row).
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    iron::EntityId c{2, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    arr.add(c, 30);
    arr.remove(b);
    CHECK(arr.size() == 2);
    CHECK(arr.get(b) == nullptr);
    CHECK(*arr.get(a) == 10);
    CHECK(*arr.get(c) == 30);  // sparse for c must be updated after the swap
}

static void test_component_array_remove_then_readd() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    arr.add(e, 7);
    arr.remove(e);
    arr.add(e, 99);
    CHECK(arr.size() == 1);
    CHECK(*arr.get(e) == 99);
}
```

Register in `main()`:
```cpp
    test_component_array_remove_invalidates_get();
    test_component_array_remove_swaps_correctly();
    test_component_array_remove_then_readd();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `arr.remove(e)` not declared.

- [ ] **Step 3: Implement `remove`**

Add to `engine/world/ComponentArray.h` inside the class, after `size()`:

```cpp
    void remove(EntityId e) {
        if (e.index >= sparse_.size()) return;
        const uint32_t row = sparse_[e.index];
        if (row == kNoRow) return;
        if (!(denseEntities_[row] == e)) return;  // stale handle

        const uint32_t last = static_cast<uint32_t>(dense_.size() - 1);
        if (row != last) {
            // Swap-and-pop: move the last entry into the freed row, then update
            // the swapped entity's sparse index to point at its new row.
            dense_[row]         = std::move(dense_[last]);
            denseEntities_[row] = denseEntities_[last];
            sparse_[denseEntities_[row].index] = row;
        }
        dense_.pop_back();
        denseEntities_.pop_back();
        sparse_[e.index] = kNoRow;
    }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/ComponentArray.h tests/test_world.cpp
git commit -m "M37: ComponentArray<T> — remove (swap-and-pop) + tests"
```

---

### Task A4: `ComponentArray<T>` — `operator[]` + `entityAt` (iteration)

**Files:**
- Modify: `engine/world/ComponentArray.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append BEFORE `int main()`:

```cpp
static void test_component_array_operator_index() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr[0] == 10);
    CHECK(arr[1] == 20);
}

static void test_component_array_entity_at() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr.entityAt(0) == a);
    CHECK(arr.entityAt(1) == b);
}

static void test_component_array_iteration_after_swap() {
    // Insert A,B,C; remove B; verify rows 0,1 are A=10, C=30.
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    iron::EntityId c{2, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    arr.add(c, 30);
    arr.remove(b);
    CHECK(arr.size() == 2);
    CHECK(arr[0] == 10);
    CHECK(arr[1] == 30);
    CHECK(arr.entityAt(0) == a);
    CHECK(arr.entityAt(1) == c);
}
```

Register in `main()`:
```cpp
    test_component_array_operator_index();
    test_component_array_entity_at();
    test_component_array_iteration_after_swap();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile errors — `arr[0]`, `arr.entityAt(...)` not declared.

- [ ] **Step 3: Implement `operator[]` + `entityAt`**

Add to `engine/world/ComponentArray.h` inside the class, after `remove()`:

```cpp
    T&       operator[](size_t denseRow)       { return dense_[denseRow]; }
    const T& operator[](size_t denseRow) const { return dense_[denseRow]; }

    EntityId entityAt(size_t denseRow) const { return denseEntities_[denseRow]; }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/ComponentArray.h tests/test_world.cpp
git commit -m "M37: ComponentArray<T> — operator[] + entityAt + tests"
```

---

## Phase B — `World` typed component API

Goal: wrap `ComponentArray<T>` in `World`, with entity lifecycle (generational `create`/`destroy`/`alive`) and typed `add<T>`/`get<T>`/`remove<T>`/`view<T>`.

### Task B1: `World` — entity lifecycle (`create` / `destroy` / `alive`)

**Files:**
- Create: `engine/world/World.h`
- Create: `engine/world/World.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append BEFORE `int main()`:

```cpp
#include "world/World.h"

static void test_world_create_returns_valid_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    CHECK(e.valid());
    CHECK(w.alive(e));
}

static void test_world_destroy_kills_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    w.destroy(e);
    CHECK(!w.alive(e));
}

static void test_world_recycle_bumps_generation() {
    iron::World w;
    iron::EntityId a = w.create();
    w.destroy(a);
    iron::EntityId b = w.create();
    CHECK(b.valid());
    CHECK(b.index == a.index);   // slot reused
    CHECK(b.generation != a.generation);
    CHECK(!w.alive(a));          // stale handle stays dead
    CHECK(w.alive(b));
}
```

Register in `main()`:
```cpp
    test_world_create_returns_valid_entity();
    test_world_destroy_kills_entity();
    test_world_recycle_bumps_generation();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `world/World.h` does not exist.

- [ ] **Step 3: Implement `World.h` (lifecycle only)**

Create `engine/world/World.h`:

```cpp
#pragma once

#include "world/Entity.h"

#include <cstdint>
#include <vector>

namespace iron {

class World {
public:
    EntityId create();
    void     destroy(EntityId e);
    bool     alive(EntityId e) const;

private:
    // generations_[i] == 0 means "slot never used"; non-zero is current gen.
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
};

}  // namespace iron
```

Create `engine/world/World.cpp`:

```cpp
#include "world/World.h"

namespace iron {

EntityId World::create() {
    uint32_t index;
    if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
        // generation was already bumped on destroy.
    } else {
        index = static_cast<uint32_t>(generations_.size());
        generations_.push_back(1);   // first-ever generation is 1
    }
    return EntityId{index, generations_[index]};
}

void World::destroy(EntityId e) {
    if (!alive(e)) return;
    // Bump generation; any stale handle that captured the old generation
    // will fail alive() and get<T>() from now on.
    ++generations_[e.index];
    if (generations_[e.index] == 0) generations_[e.index] = 1;  // wrap guard
    freeList_.push_back(e.index);
}

bool World::alive(EntityId e) const {
    if (e.index >= generations_.size()) return false;
    return e.valid() && generations_[e.index] == e.generation;
}

}  // namespace iron
```

Modify `engine/CMakeLists.txt` — add `world/World.cpp` to the `ironcore` source list (alphabetical position: after `util/FileWatcher.cpp`, before `debug/GizmoRegistry.cpp` works; OR append at the end of the list — both compile):

```cmake
  world/World.cpp
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/World.h engine/world/World.cpp engine/CMakeLists.txt tests/test_world.cpp
git commit -m "M37: World — entity lifecycle (create/destroy/alive) + tests"
```

---

### Task B2: `World` — typed `add<T>` / `get<T>` / `remove<T>` (type-erased arrays)

**Files:**
- Modify: `engine/world/World.h`
- Modify: `engine/world/World.cpp`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append BEFORE `int main()`:

```cpp
static void test_world_typed_add_and_get() {
    iron::World w;
    iron::EntityId e = w.create();
    int* p = w.add<int>(e, 42);
    CHECK(p != nullptr);
    CHECK(*w.get<int>(e) == 42);
}

static void test_world_typed_remove() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 42);
    w.remove<int>(e);
    CHECK(w.get<int>(e) == nullptr);
}

static void test_world_multi_type_on_same_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 7);
    w.add<float>(e, 3.5f);
    CHECK(*w.get<int>(e) == 7);
    CHECK(*w.get<float>(e) == 3.5f);
}

static void test_world_destroy_tears_off_all_components() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 7);
    w.add<float>(e, 3.5f);
    w.destroy(e);
    CHECK(w.get<int>(e) == nullptr);
    CHECK(w.get<float>(e) == nullptr);
}
```

Register in `main()`:
```cpp
    test_world_typed_add_and_get();
    test_world_typed_remove();
    test_world_multi_type_on_same_entity();
    test_world_destroy_tears_off_all_components();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile errors — `w.add<int>`, `w.get<int>`, `w.remove<int>` not declared.

- [ ] **Step 3: Implement typed component API**

Modify `engine/world/World.h` — add the typed API and type-erased base. Replace the existing class body with:

```cpp
#pragma once

#include "world/ComponentArray.h"
#include "world/Entity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace iron {

class World {
public:
    static constexpr uint32_t kMaxComponentTypes = 256;

    EntityId create();
    void     destroy(EntityId e);
    bool     alive(EntityId e) const;

    template <class T> T*       add(EntityId e, const T& v = {}) {
        return arrayFor<T>().add(e, v);
    }
    template <class T> T*       get(EntityId e) {
        if (!alive(e)) return nullptr;
        auto* a = tryArrayFor<T>();
        return a ? a->get(e) : nullptr;
    }
    template <class T> const T* get(EntityId e) const {
        return const_cast<World*>(this)->get<T>(e);
    }
    template <class T> void remove(EntityId e) {
        if (auto* a = tryArrayFor<T>()) a->remove(e);
    }

private:
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void remove(EntityId e) = 0;   // type-erased remove for destroy()
    };

    template <class T>
    struct TypedComponentArray : IComponentArray, ComponentArray<T> {
        void remove(EntityId e) override { ComponentArray<T>::remove(e); }
    };

    template <class T>
    TypedComponentArray<T>& arrayFor() {
        const uint32_t id = componentTypeId<T>();
        if (!arrays_[id]) arrays_[id] = std::make_unique<TypedComponentArray<T>>();
        return static_cast<TypedComponentArray<T>&>(*arrays_[id]);
    }

    template <class T>
    TypedComponentArray<T>* tryArrayFor() {
        const uint32_t id = componentTypeId<T>();
        return arrays_[id] ? static_cast<TypedComponentArray<T>*>(arrays_[id].get())
                           : nullptr;
    }

    std::array<std::unique_ptr<IComponentArray>, kMaxComponentTypes> arrays_{};
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
};

}  // namespace iron
```

Modify `engine/world/World.cpp` — extend `destroy` to tear off all components before bumping the generation:

```cpp
void World::destroy(EntityId e) {
    if (!alive(e)) return;
    // Tear off every component first so swap-and-pop fixups happen against
    // the still-valid handle.
    for (auto& a : arrays_) {
        if (a) a->remove(e);
    }
    ++generations_[e.index];
    if (generations_[e.index] == 0) generations_[e.index] = 1;  // wrap guard
    freeList_.push_back(e.index);
}
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all `test_world` tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/World.h engine/world/World.cpp tests/test_world.cpp
git commit -m "M37: World — typed add/get/remove<T> via type-erased ComponentArray"
```

---

### Task B3: `World::view<T>()` iteration

**Files:**
- Modify: `engine/world/World.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing tests**

Append BEFORE `int main()`:

```cpp
static void test_world_view_size_matches_components_present() {
    iron::World w;
    iron::EntityId a = w.create();
    iron::EntityId b = w.create();
    w.add<int>(a, 10);
    w.add<int>(b, 20);
    auto& v = w.view<int>();
    CHECK(v.size() == 2);
}

static void test_world_view_iteration_and_entity_at() {
    iron::World w;
    iron::EntityId a = w.create();
    iron::EntityId b = w.create();
    w.add<int>(a, 10);
    w.add<int>(b, 20);
    auto& v = w.view<int>();
    CHECK(v[0] == 10 || v[0] == 20);   // order is insertion-modulo-swaps
    CHECK(v.entityAt(0) == a || v.entityAt(0) == b);
}

static void test_world_view_empty_when_no_component_of_type() {
    iron::World w;
    auto& v = w.view<int>();
    CHECK(v.size() == 0);
}
```

Register in `main()`:
```cpp
    test_world_view_size_matches_components_present();
    test_world_view_iteration_and_entity_at();
    test_world_view_empty_when_no_component_of_type();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `w.view<int>()` not declared.

- [ ] **Step 3: Implement `view<T>`**

Add to `engine/world/World.h` inside the class, after `remove<T>`:

```cpp
    template <class T>
    ComponentArray<T>& view() {
        return arrayFor<T>();
    }

    template <class T>
    const ComponentArray<T>& view() const {
        const auto* a = const_cast<World*>(this)->tryArrayFor<T>();
        if (a) return *a;
        // Lazy-create empty array so const view() always returns a valid ref.
        const_cast<World*>(this)->arrayFor<T>();
        return *const_cast<World*>(this)->tryArrayFor<T>();
    }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/World.h tests/test_world.cpp
git commit -m "M37: World::view<T>() iteration + tests"
```

---

## Phase C — POD component types + render-submit integration test

Goal: define `Transform` and `RenderHandles`, prove a full World can carry every component a renderable entity needs, walk the Section 3 submit pseudocode in a test.

### Task C1: `Transform` component

**Files:**
- Create: `engine/world/Transform.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing test**

Append BEFORE `int main()`:

```cpp
#include "world/Transform.h"

static void test_transform_component_roundtrip() {
    iron::World w;
    iron::EntityId e = w.create();
    iron::Transform t{};
    t.position = iron::Vec3{1, 2, 3};
    t.scale    = iron::Vec3{2, 2, 2};
    w.add<iron::Transform>(e, t);
    auto* got = w.get<iron::Transform>(e);
    CHECK(got != nullptr);
    CHECK(got->position.x == 1.0f);
    CHECK(got->position.y == 2.0f);
    CHECK(got->position.z == 3.0f);
    CHECK(got->scale.x    == 2.0f);
}
```

Register in `main()`:
```cpp
    test_transform_component_roundtrip();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `world/Transform.h` does not exist.

- [ ] **Step 3: Implement `Transform.h`**

Create `engine/world/Transform.h`:

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"

namespace iron {

struct Transform {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale    = {1.0f, 1.0f, 1.0f};
};

}  // namespace iron
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/world/Transform.h tests/test_world.cpp
git commit -m "M37: Transform component + roundtrip test"
```

---

### Task C2: `RenderHandles` component

**Files:**
- Create: `engine/render/RenderHandles.h`
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing test**

Append BEFORE `int main()`:

```cpp
#include "render/RenderHandles.h"

static void test_render_handles_component_roundtrip() {
    iron::World w;
    iron::EntityId e = w.create();
    iron::RenderHandles rh{};
    rh.mesh     = 7;
    rh.albedo   = 11;
    rh.normal   = 13;
    rh.specular = 17;
    w.add<iron::RenderHandles>(e, rh);
    auto* got = w.get<iron::RenderHandles>(e);
    CHECK(got != nullptr);
    CHECK(got->mesh   == 7u);
    CHECK(got->albedo == 11u);
    CHECK(got->normal == 13u);
    CHECK(got->specular == 17u);
}
```

Register in `main()`:
```cpp
    test_render_handles_component_roundtrip();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `render/RenderHandles.h` does not exist.

- [ ] **Step 3: Implement `RenderHandles.h`**

Create `engine/render/RenderHandles.h`:

```cpp
#pragma once

#include "render/Handles.h"

namespace iron {

struct RenderHandles {
    MeshHandle    mesh     = 0;
    TextureHandle albedo   = 0;
    TextureHandle normal   = 0;
    TextureHandle specular = 0;
};

}  // namespace iron
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add engine/render/RenderHandles.h tests/test_world.cpp
git commit -m "M37: RenderHandles component + roundtrip test"
```

---

### Task C3: Render-submit pseudocode integration test

**Files:**
- Modify: `tests/test_world.cpp`

- [ ] **Step 1: Write the failing test**

Append BEFORE `int main()`. This test walks the Section 3 submit pseudocode against three entities and records (Transform, MaterialDef, RenderHandles) tuples to assert the order:

```cpp
#include "scene/SceneFormat.h"   // MeshRef, MaterialDef

static void test_world_render_submit_pseudocode() {
    iron::World w;
    struct SubmitEntry {
        iron::Vec3       pos;
        iron::MeshHandle mesh;
    };
    std::vector<SubmitEntry> submitted;

    // Build three renderable entities.
    auto makeEntity = [&](iron::Vec3 pos, iron::MeshHandle m,
                          iron::TextureHandle a, float emissiveR) {
        iron::EntityId e = w.create();
        iron::Transform t{};      t.position = pos;
        w.add<iron::Transform>(e, t);
        iron::MeshRef ref{};      // empty primitive/path — fine for the test
        w.add<iron::MeshRef>(e, ref);
        iron::MaterialDef mat{};  mat.emissive = iron::Vec3{emissiveR, 0, 0};
        w.add<iron::MaterialDef>(e, mat);
        iron::RenderHandles rh{}; rh.mesh = m; rh.albedo = a;
        w.add<iron::RenderHandles>(e, rh);
    };
    makeEntity(iron::Vec3{1, 0, 0}, 100, 200, 0.1f);
    makeEntity(iron::Vec3{2, 0, 0}, 101, 201, 0.2f);
    makeEntity(iron::Vec3{3, 0, 0}, 102, 202, 0.3f);

    // Walk Section 3 submit pseudocode: iterate Transform view, look up siblings.
    auto& transforms = w.view<iron::Transform>();
    for (size_t row = 0; row < transforms.size(); ++row) {
        iron::EntityId e = transforms.entityAt(row);
        const iron::Transform&     t   = transforms[row];
        const iron::MeshRef*       mr  = w.get<iron::MeshRef>(e);
        const iron::MaterialDef*   mat = w.get<iron::MaterialDef>(e);
        const iron::RenderHandles* rh  = w.get<iron::RenderHandles>(e);
        if (!mr || !mat || !rh) continue;
        submitted.push_back({t.position, rh->mesh});
    }

    CHECK(submitted.size() == 3);
    CHECK(submitted[0].pos.x  == 1.0f);
    CHECK(submitted[0].mesh   == 100u);
    CHECK(submitted[2].mesh   == 102u);
}
```

Register in `main()`:
```cpp
    test_world_render_submit_pseudocode();
```

- [ ] **Step 2: Run + verify pass**

```bash
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: all `test_world` tests pass — this exercises the whole World stack end-to-end.

- [ ] **Step 3: Run full suite — confirm no regressions**

```bash
cd build-vk && ctest -C Debug --output-on-failure
```
Expected: prior 47 tests + the now-grown `test_world` all pass. Sanity-check the `test_world` test count climbed by every test added in Phases A–C.

- [ ] **Step 4: Commit**

```bash
git add tests/test_world.cpp
git commit -m "M37: render-submit pseudocode integration test (Transform+MeshRef+MaterialDef+RenderHandles)"
```

---

## Phase D — Sandbox integration

Goal: the sandbox builds a `World` on load, the per-frame submit loop iterates it, add / duplicate / delete mirror to the World, and an end-of-frame Inspector → World sync keeps editor edits visible. Renderer code is untouched.

### Task D1: Build the World on load + refactor `resolveEntity` to return `RenderHandles`

**Files:**
- Modify: `games/11-sandbox/main.cpp`

**Context to read first:**
- The current `resolveEntity` helper inside `main.cpp` (search for `resolveEntity(`).
- The current submit loop (search for `re.entityIndex` and `renderer.submit`).
- The startup load that calls `resolveEntity` in a loop over `scene.entities`.

- [ ] **Step 1: Add World member + sceneIndexToEntity_ vector**

Near the other host-owned state (after `iron::SceneFile scene;` and `int selectedIndex = ...;`), add:

```cpp
#include "world/World.h"
#include "world/Transform.h"
#include "render/RenderHandles.h"
// ...
iron::World world;
std::vector<iron::EntityId> sceneIndexToEntity;
```

- [ ] **Step 2: Refactor `resolveEntity` to return `RenderHandles`**

Find the existing `resolveEntity(SceneEntity&, ResolvedEntity&)` (or similarly-named) lambda/function. Change its signature to return `iron::RenderHandles`:

```cpp
// Old:
// bool resolveEntity(const iron::SceneEntity& se, int sceneIdx, iron::ResolvedEntity& out);
// New:
auto resolveEntity = [&](const iron::SceneEntity& se) -> iron::RenderHandles {
    iron::RenderHandles rh{};
    // ... existing mesh+texture loading code, but writing into rh.mesh / rh.albedo / ...
    //     instead of out.meshHandle / out.albedoHandle / ...
    return rh;
};
```

(Keep the existing `resolved` vector AND `ResolvedEntity` for now — Task D2 removes them.)

- [ ] **Step 3: At each startup-load step, also build the World entity**

In the loop that processes `scene.entities`, alongside the existing `resolveEntity` call, add:

```cpp
for (size_t i = 0; i < scene.entities.size(); ++i) {
    const iron::SceneEntity& se = scene.entities[i];
    iron::ResolvedEntity re;
    if (!resolveEntityLegacy(se, static_cast<int>(i), re)) continue;  // existing path
    resolved.push_back(re);

    // NEW: mirror into the World.
    iron::EntityId e = world.create();
    iron::Transform t{}; t.position = se.position; t.rotation = se.rotation; t.scale = se.scale;
    world.add<iron::Transform>(e, t);
    world.add<iron::MeshRef>(e, se.mesh);
    world.add<iron::MaterialDef>(e, se.material);
    iron::RenderHandles rh = resolveEntity(se);   // new lambda
    world.add<iron::RenderHandles>(e, rh);
    sceneIndexToEntity.push_back(e);
}
```

**Naming rule** (so the two lambdas don't shadow): rename the existing legacy lambda to `resolveEntityLegacy` (it still writes a `ResolvedEntity` for the now-doomed `resolved[]` vector). Add the NEW lambda named `resolveEntity` that returns `RenderHandles`. Task D2 deletes `resolveEntityLegacy` and `resolved[]` together; the new `resolveEntity` survives.

- [ ] **Step 4: Build the sandbox**

```bash
cmake --build build-vk --config Debug --target sandbox
```
Expected: clean build. (LNK4217 ImGui warnings are benign.)

- [ ] **Step 5: Run the sandbox and confirm nothing visible changed**

```bash
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: identical to before — scene renders exactly as before because the existing submit loop still uses `resolved[]`. Close the window via X or Esc-deselect (Esc does not close — see M36 sandbox edit).

- [ ] **Step 6: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M37: sandbox — build World on load + resolveEntity returns RenderHandles"
```

---

### Task D2: Sandbox submit loop reads from World; delete `resolved[]`

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Replace the submit loop**

Find the per-frame submit loop. It currently iterates `resolved[]` to build DrawCalls. Replace with:

```cpp
auto& transforms = world.view<iron::Transform>();
for (size_t row = 0; row < transforms.size(); ++row) {
    iron::EntityId e = transforms.entityAt(row);
    const iron::Transform&     t   = transforms[row];
    const iron::MeshRef*       mr  = world.get<iron::MeshRef>(e);
    const iron::MaterialDef*   mat = world.get<iron::MaterialDef>(e);
    const iron::RenderHandles* rh  = world.get<iron::RenderHandles>(e);
    if (!mr || !mat || !rh) continue;

    iron::DrawCall call{};
    // Build the model matrix from t.position / t.rotation / t.scale.
    // Find the exact expression the legacy submit loop used by greping for
    // `call.model = ` in main.cpp and copy that line verbatim, replacing
    // `se.position`/`se.rotation`/`se.scale` with `t.position`/`t.rotation`/`t.scale`.
    call.model      = /* see grep instruction above */;
    call.mesh       = rh->mesh;
    call.albedo     = rh->albedo;
    call.normal     = rh->normal;
    call.specular   = rh->specular;
    call.emissive   = mat->emissive;
    call.uvScale    = mat->uvScale;
    call.reflectivity = mat->reflectivity;
    // effectId stays whatever the existing per-entity selection logic sets — keep that block.
    call.effectId   = (entityIndexFor(e) == selectedIndex) ? 1 : 0;   // see Step 2

    renderer.submit(call, view, proj);
}
```

(Use the helpers / model-matrix construction the old loop did — don't reinvent. The point is the iteration source, not the call shape.)

- [ ] **Step 2: Implement an `entityIndexFor(EntityId)` lookup**

`selectedIndex` is still an `int` indexing `scene.entities[]`. The submit loop needs to know "is this World entity the selected one?" Add a small reverse lookup before the submit loop or alongside `sceneIndexToEntity`:

```cpp
auto entityIndexFor = [&](iron::EntityId e) -> int {
    for (size_t i = 0; i < sceneIndexToEntity.size(); ++i)
        if (sceneIndexToEntity[i] == e) return static_cast<int>(i);
    return -1;
};
```

(O(N) but N is tiny; cleaner shapes land in M39.)

- [ ] **Step 3: Delete the `resolved[]` vector and `ResolvedEntity` references**

Remove the `std::vector<iron::ResolvedEntity> resolved;` member, every `resolved.push_back(...)`, `resolved.erase(...)`, and any references to `re.entityIndex` / `re.meshHandle` etc. in the now-replaced submit loop. The legacy `resolveEntityLegacy` (or however you renamed it in D1 Step 2) goes too — only the new `resolveEntity` lambda remains.

- [ ] **Step 4: Build and run**

```bash
cmake --build build-vk --config Debug --target sandbox
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: scene renders identically. Selection effects still work (you set the orange outline as the M36 default — click an entity, the outline appears).

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M37: sandbox submit loop reads from World; resolved[] removed"
```

---

### Task D3: Sandbox add / duplicate / delete mirror to the World

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: In the add path (cube/plane/glTF), mirror into the World**

Find the existing add-entity handler (search for `OutlinerResult::Action::AddCube`). At the end of the add path, after the new `SceneEntity` has been pushed to `scene.entities` and the existing resolve happens:

```cpp
iron::EntityId e = world.create();
iron::Transform t{};
t.position = scene.entities.back().position;
t.rotation = scene.entities.back().rotation;
t.scale    = scene.entities.back().scale;
world.add<iron::Transform>(e, t);
world.add<iron::MeshRef>(e, scene.entities.back().mesh);
world.add<iron::MaterialDef>(e, scene.entities.back().material);
world.add<iron::RenderHandles>(e, resolveEntity(scene.entities.back()));
sceneIndexToEntity.push_back(e);
```

The duplicate path goes through the same code path (it pushes a copy of `scene.entities[selectedIndex]`), so this single block covers add + duplicate.

- [ ] **Step 2: In the delete path, destroy the World entity + reindex `sceneIndexToEntity`**

Find the existing delete handler (search for `OutlinerResult::Action::Delete`). It currently `erase`s from `scene.entities` and `resolved` (the latter is gone after D2). Add:

```cpp
const int d = selectedIndex;
iron::EntityId e = sceneIndexToEntity[d];
world.destroy(e);
sceneIndexToEntity.erase(sceneIndexToEntity.begin() + d);
// sceneIndexToEntity is parallel to scene.entities; no reindex needed beyond erase.
```

- [ ] **Step 3: Build and run**

```bash
cmake --build build-vk --config Debug --target sandbox
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: add a cube — it appears. Duplicate selected entity — copy appears. Delete — it disappears. Selection effect still picks the right entity after each operation.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M37: sandbox add/duplicate/delete mirror to World"
```

---

### Task D4: Sandbox end-of-frame Inspector → World sync

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Add the sync loop after editor panels run, before `renderer.beginFrame`**

The Inspector and the Gizmo edit `scene.entities[selectedIndex]` directly. Without this loop, the World's `Transform` / `MaterialDef` go stale and the rendered entity stops following the edits. Add (after `inspector.draw(...)` / `gizmo.update(...)` / etc., before the submit loop):

```cpp
// --- M37: Inspector/Gizmo edits scene.entities[]; mirror them into the World. ---
for (size_t i = 0; i < scene.entities.size(); ++i) {
    const iron::SceneEntity& se = scene.entities[i];
    if (i >= sceneIndexToEntity.size()) break;
    iron::EntityId e = sceneIndexToEntity[i];
    if (auto* t = world.get<iron::Transform>(e)) {
        t->position = se.position;
        t->rotation = se.rotation;
        t->scale    = se.scale;
    }
    if (auto* m = world.get<iron::MaterialDef>(e)) {
        *m = se.material;
    }
    // MeshRef does not change at runtime in v1; skip.
}
```

- [ ] **Step 2: Build and run**

```bash
cmake --build build-vk --config Debug --target sandbox
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: select an entity, drag the translate/rotate/scale gizmo — the entity follows live. Edit emissive / uvScale / reflectivity in the Inspector — visible.

- [ ] **Step 3: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M37: sandbox end-of-frame Inspector -> World sync"
```

---

## Phase E — Verification, PR, merge

### Task E1: Full-suite green + visual verification gate

- [ ] **Step 1: Run the full test suite**

```bash
cmake --build build-vk --config Debug
cd build-vk && ctest -C Debug --output-on-failure
```
Expected: 47 prior tests + `test_world` all green. `test_world` reports its own count (should be roughly 18–20 named subtests collapsed into a single CTest case).

- [ ] **Step 2: Run the sandbox and walk the checklist**

```bash
./build-vk/games/11-sandbox/Debug/sandbox.exe
```

Visual gate — confirm each works exactly as on `main`:

| Action | Expected |
|---|---|
| Scene loads from `demo.json` | All entities render at the right positions |
| Left-click an entity | Selected entity gets orange outline (M36) |
| Drag translate gizmo | Entity moves live |
| Drag rotate gizmo | Entity rotates live, outline follows (M35 oriented outline) |
| Drag scale gizmo | Entity scales live |
| Inspector edit emissive | Visible glow change |
| Add cube | New cube appears at camera ~5 units forward, selected |
| Duplicate selected | Copy appears at the same spot, selected |
| Delete (Ctrl+D / Delete) | Entity disappears, selection moves to -1 |
| Esc | Deselects (M36) |
| Switch Selection Effect in Inspector | Effect changes live |

If any of these regress, return to the corresponding D-phase task and fix.

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/m37-component-model
```

- [ ] **Step 4: Open the PR**

```bash
gh pr create --title "M37: component model — World + Transform/MeshRef/MaterialDef/RenderHandles" --body "$(cat <<'EOF'
## Summary

M37 — first milestone of the foundation track. Introduces `iron::World`
(entity-component runtime container) and ports the renderable-entity
data shape into components. The sandbox now renders by iterating the
World; the editor (Outliner / Inspector / Gizmo / picking) still
operates on `SceneFile` with an end-of-frame Inspector -> World sync
until M39 migrates it.

- **New module** `engine/world/`: `EntityId` (generational 32+32),
  `ComponentArray<T>` (dense vec + sparse index, swap-and-pop remove),
  `World` (type-erased typed `add/get/remove/view<T>`).
- **Four POD component types**: `Transform` (new), `MeshRef` and
  `MaterialDef` (reused from `SceneFormat.h`), `RenderHandles` (new —
  GPU handles live on the entity, not in a parallel host cache).
- **Sandbox**: builds the World on load; render-submit iterates the
  World; add / duplicate / delete mirror; end-of-frame sync.
- **Tests**: new `test_world.cpp` covers `ComponentArray`, `World`
  lifecycle, multi-type add/destroy, view iteration, and a
  render-submit pseudocode integration test.

## Test plan

- [x] Full suite green (48/48 — `test_world` adds one CTest case
  covering all the new unit tests)
- [x] ironcore + sandbox build clean
- [x] Visual: scene renders identically; gizmo edits follow live;
  add/duplicate/delete still work; selection effect picks the right
  entity

## Known v1 limitations (deferred to M38+)

- Reflection / generic component serialization — M38.
- Editor add/remove arbitrary components — M39.
- Migrating Outliner / Inspector / Gizmo / picking off `SceneFile` —
  M39 (the Inspector -> World sync is the bridging hack).
- Class-with-methods user-component layer (the hybrid's OO half) — M40
  alongside AngelScript.
- Archetype storage / multi-component queries — defer; current shape is
  fine at TF2/Overwatch entity scale.
EOF
)"
```

- [ ] **Step 5: Watch CI and squash-merge once green**

```bash
gh pr checks --watch
gh pr merge --squash --delete-branch
git checkout main && git pull --ff-only origin main
```

- [ ] **Step 6: Update memory**

After the merge:
- Append an `M37 — component model` entry to `iron-core-engine-progress.md`.
- Mark M37 done in `iron-core-engine-roadmap.md` (foundation track now started).
- Bump the `MEMORY.md` index `iron-core-engine-progress` line to reference the M37 squash-merge SHA.

---

## Acceptance criteria

1. `iron::World` exists, owns `Transform` / `MeshRef` / `MaterialDef` / `RenderHandles`, passes `test_world`.
2. The sandbox renders by iterating the World; the host-side `resolved[]` cache is gone.
3. Add / duplicate / delete keep `scene.entities[]`, `sceneIndexToEntity`, and the World in sync.
4. Inspector / Gizmo edits propagate to render via the end-of-frame sync; visual gate green.
5. 47 → 48 tests green (single `test_world` CTest case covers all new unit tests).
6. Zero new external dependencies; renderer backend (Vulkan) untouched.
7. PR merged; memory updated.
