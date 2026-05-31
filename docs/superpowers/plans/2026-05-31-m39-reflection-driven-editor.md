# M39 — Editor Inspector + SceneIO migrate onto `iron::Reflection` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the editor Inspector render data-driven from `iron::Reflection`, switch `SceneIO` to serialize through the same registry, and unify `SceneEntity` with M37's `Transform` component (drop the duplicated position/rotation/scale fields).

**Architecture:** Two new generic helpers (`renderComponent<T>` in the editor, `componentToJson<T>` / `componentFromJson<T>` in SceneIO) dispatch on `TypeId` and ImGui-or-JSON-render any field of any registered component. `Reflection` gains an `EnumBuilder` so dropdowns and enum strings are driven by the registry. `FieldMeta` gains three widget hints (`dragSpeed`, `color`, `slider`); `FieldDesc` gains a `enumTypeId` so the dispatch layer can look up enum value lists without knowing the concrete `E`. No legacy format fallback — `games/11-sandbox/assets/scenes/demo.json` is hand-migrated to the new nested format in Task B1.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), `nlohmann/json` (already vendored), Dear ImGui (already linked into `ironcore_editor`). No new external dependencies. Reference spec: `docs/superpowers/specs/2026-05-31-m39-reflection-driven-editor-design.md`.

**Verification model:** Pure-logic phases (A, C2) are TDD via `test_type_reflection` / new `test_reflection_io`. Refactor phases (B1, C1, C3, D1) gate on "build clean + 49 / 50 / 50 / 50 existing tests stay green". Phase E1 adds the visual gate (sandbox still works, mesh dropdown now editable).

**Build & test commands (used by every task):**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_type_reflection
cmake --build build-vk --config Debug --target test_reflection_io   # after Task C2 lands
cmake --build build-vk --config Debug --target sandbox              # for D1/E1
ctest --test-dir build-vk -C Debug --output-on-failure
```
(A benign "LF will be replaced by CRLF" git warning is expected on Windows. Pre-existing ImGui/GLFW `LNK4217` linker warnings are benign.)

**Branch:** already on `feat/m39-reflection-driven-editor` (spec commit `86eaee8` sits on the branch tip). Every task in this plan commits to this branch.

---

## File Structure

**New (engine):**
- `engine/scene/ReflectionIO.h` — header-only template forwarders: `componentToJson<T>` / `componentFromJson<T>`
- `engine/scene/ReflectionIO.cpp` — non-template byte-pointer workers + `TypeId` dispatch table
- `engine/editor/ReflectionInspector.h` — header-only template forwarder: `renderComponent<T>`
- `engine/editor/ReflectionInspector.cpp` — non-template byte-pointer worker + `TypeId` dispatch (ImGui kept private to this TU)

**New (tests):**
- `tests/test_reflection_io.cpp` — ~10 named subtests for `componentToJson` / `componentFromJson` roundtrips

**Modified (engine):**
- `engine/reflection/FieldDesc.h` — `FieldMeta` gains `dragSpeed`, `color`, `slider`; `FieldDesc` gains `enumTypeId`
- `engine/reflection/TypeIdOf.h` — add `is_optional_enum_v` + `enumTypeIdOf<F>()` helpers
- `engine/reflection/Reflection.h` — `TypeBuilder::field` populates `enumTypeId`; add `EnumBuilder<E>`, `registerEnum<E>`, `enumValues<E>`, `enumName<E>`, non-template `enumValuesById` / `enumNameById`
- `engine/scene/SceneFormat.h` — `SceneEntity` contains `Transform transform` (drop bare position/rotation/scale)
- `engine/scene/SceneIO.h` — `loadSceneFile` / `saveSceneFile` take `const Reflection&`
- `engine/scene/SceneIO.cpp` — entity ser/deser switches to `componentToJson` / `componentFromJson`; sun/fog/lights/clearColor stay hand-rolled
- `engine/scene/MeshRef.reflect.cpp` — add `registerEnum<PrimitiveKind>("PrimitiveKind").value("cube", …).value("plane", …);` before the type registration
- `engine/scene/MaterialDef.reflect.cpp` — add `.color = true` on `emissive`, `.slider = true` on `reflectivity`
- `engine/editor/SceneInspector.h` — `draw` gains `const Reflection&` first parameter
- `engine/editor/SceneInspector.cpp` — entity body collapses to three `renderComponent` calls; editor-tool prelude (gizmo space, effect picker) stays hand-rolled
- `engine/editor/Gizmo.cpp` — `e.position` / `e.rotation` / `e.scale` → `e.transform.*` (10 occurrences)
- `engine/CMakeLists.txt` — append `editor/ReflectionInspector.cpp` and `scene/ReflectionIO.cpp` to the `ironcore` source list

**Modified (tests):**
- `tests/CMakeLists.txt` — `iron_add_test(test_reflection_io test_reflection_io.cpp)`
- `tests/test_type_reflection.cpp` — append subtests for the new schema fields (A1–A3)
- `tests/test_scene_io.cpp` — port to the new nested format; construct + pass `Reflection&`; rename `e.position` → `e.transform.position` etc.

**Modified (games):**
- `games/11-sandbox/main.cpp` — pass `reflection` ref to `inspector.draw` and `loadSceneFile` / `saveSceneFile`; rename `e.position` → `e.transform.position` everywhere
- `games/11-sandbox/assets/scenes/demo.json` — hand-migrate flat `position` / `rotation` / `scale` per entity into nested `"transform": {…}`

**Untouched on purpose:** Renderer (Vulkan), World / ComponentArray, picking, Outliner (its add/delete/duplicate paths only mutate `scene.entities` — the SceneEntity reshape is transparent to them), shipping games (`net-shooter`, `02-strandbound`, `04-net-pingpong`, `05-net-cubes`, `06-net-tag`, `09-physics-playground`, etc.).

**Phases:**
- **A** — Reflection schema extension. Pure logic, TDD. 3 tasks.
- **B** — `SceneEntity` contains `Transform` (mechanical rename). Refactor. 1 task.
- **C** — Reflection-driven helpers + sidecar hint updates. Mostly pure logic, TDD where automatable. 3 tasks.
- **D** — Wire-up: `SceneInspector` body, `SceneIO` entity ser/deser, sandbox host. 1 task.
- **E** — Visual gate + push + PR + squash-merge + memory. 1 task.

Total: 9 tasks.

---

## Phase A — Reflection schema extension (pure logic, TDD)

### Task A1: `FieldMeta` gains widget hints (`dragSpeed`, `color`, `slider`)

**Files:**
- Modify: `engine/reflection/FieldDesc.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
static void test_fieldmeta_widget_hint_defaults() {
    iron::FieldMeta m;
    CHECK(m.dragSpeed == 0.0f);
    CHECK(m.color  == false);
    CHECK(m.slider == false);
}

static void test_field_records_widget_hints() {
    struct Probe { float a; iron::Vec3 b; };
    iron::Reflection r;
    r.registerType<Probe>("Probe")
        .field("a", &Probe::a, {.min = 0.0f, .max = 1.0f, .slider = true})
        .field("b", &Probe::b, {.color = true});
    auto fields = r.fieldsOf<Probe>();
    CHECK(fields.size() == 2);
    CHECK(fields[0].meta.slider == true);
    CHECK(fields[0].meta.min == 0.0f);
    CHECK(fields[0].meta.max == 1.0f);
    CHECK(fields[1].meta.color == true);
    CHECK(fields[1].meta.dragSpeed == 0.0f);  // default
}
```

Register both calls in `main()` after the existing tests, before the success print:

```cpp
    test_fieldmeta_widget_hint_defaults();
    test_field_records_widget_hints();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `iron::FieldMeta` has no `dragSpeed` / `color` / `slider` member.

- [ ] **Step 3: Extend `FieldMeta`**

Edit `engine/reflection/FieldDesc.h`. Replace the `FieldMeta` struct (lines 11–14):

```cpp
// Per-field metadata. v1 covers range clamps + widget hints used by the
// Inspector dispatch and ignored by SceneIO.
struct FieldMeta {
    float min       = 0.0f;   // both zero = no clamp
    float max       = 0.0f;
    float dragSpeed = 0.0f;   // 0 = Inspector picks default per TypeId
    bool  color     = false;  // Vec3 → ColorEdit3 instead of DragFloat3
    bool  slider    = false;  // float → SliderFloat instead of DragFloat (needs min+max)
};
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: `All reflection tests passed.` and `Passed`.

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/FieldDesc.h tests/test_type_reflection.cpp
git commit -m "M39: FieldMeta gains dragSpeed/color/slider widget hints"
```

Run `git log --oneline -3` and include the SHA in your report.

---

### Task A2: `FieldDesc::enumTypeId` + `TypeIdOf` enum-id helpers

**Files:**
- Modify: `engine/reflection/FieldDesc.h`
- Modify: `engine/reflection/TypeIdOf.h`
- Modify: `engine/reflection/Reflection.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
static void test_enum_typeidof_helper_returns_zero_for_non_enum() {
    CHECK(iron::enumTypeIdOf<float>()       == 0u);
    CHECK(iron::enumTypeIdOf<int32_t>()     == 0u);
    CHECK(iron::enumTypeIdOf<iron::Vec3>()  == 0u);
    CHECK(iron::enumTypeIdOf<std::string>() == 0u);
}

static void test_enum_typeidof_helper_returns_componenttypeid_for_enum() {
    const uint32_t a = iron::enumTypeIdOf<iron::PrimitiveKind>();
    const uint32_t b = iron::componentTypeId<iron::PrimitiveKind>();
    CHECK(a == b);
    CHECK(a != 0u);
}

static void test_enum_typeidof_helper_unwraps_optional() {
    const uint32_t a = iron::enumTypeIdOf<std::optional<iron::PrimitiveKind>>();
    const uint32_t b = iron::componentTypeId<iron::PrimitiveKind>();
    CHECK(a == b);
}

static void test_field_stores_enum_typeid() {
    struct Probe {
        float                              a;
        iron::PrimitiveKind                b;
        std::optional<iron::PrimitiveKind> c;
    };
    iron::Reflection r;
    r.registerType<Probe>("Probe")
        .field("a", &Probe::a)
        .field("b", &Probe::b)
        .field("c", &Probe::c);
    auto fields = r.fieldsOf<Probe>();
    CHECK(fields.size() == 3);
    CHECK(fields[0].enumTypeId == 0u);
    CHECK(fields[1].enumTypeId == iron::componentTypeId<iron::PrimitiveKind>());
    CHECK(fields[2].enumTypeId == iron::componentTypeId<iron::PrimitiveKind>());
}
```

Register in `main()`:

```cpp
    test_enum_typeidof_helper_returns_zero_for_non_enum();
    test_enum_typeidof_helper_returns_componenttypeid_for_enum();
    test_enum_typeidof_helper_unwraps_optional();
    test_field_stores_enum_typeid();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `iron::enumTypeIdOf` not declared and `FieldDesc` has no `enumTypeId` member.

- [ ] **Step 3: Extend `TypeIdOf.h` with the enum-id helpers**

Edit `engine/reflection/TypeIdOf.h`. Append BEFORE the closing `}  // namespace iron` (after the existing `TypeIdOf<std::optional<E>>` specialization):

```cpp
// Detect std::optional<E> where E is an enum (e.g. MeshRef::primitive).
template <class F>
inline constexpr bool is_optional_enum_v = false;

template <class E>
inline constexpr bool is_optional_enum_v<std::optional<E>> = std::is_enum_v<E>;

// Return the enum's registry id for enum-bearing field types; 0 otherwise.
// Used by the Inspector / SceneIO dispatch to look up value names without
// knowing the concrete enum type E.
template <class F>
constexpr uint32_t enumTypeIdOf() {
    if constexpr (std::is_enum_v<F>)
        return componentTypeId<F>();
    else if constexpr (is_optional_enum_v<F>)
        return componentTypeId<typename F::value_type>();
    else
        return 0u;
}
```

`componentTypeId<T>()` is declared in `world/Entity.h` (which `Reflection.h` already includes). `TypeIdOf.h` does not currently include it — add `#include "world/Entity.h"` at the top:

```cpp
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/TypeId.h"
#include "world/Entity.h"   // componentTypeId<T>()
```

- [ ] **Step 4: Add `enumTypeId` to `FieldDesc`**

Edit `engine/reflection/FieldDesc.h`. Replace the `FieldDesc` struct (the part declaring members; keep `ptr<T>` templates as-is):

```cpp
// One reflected field. Name has static storage duration (string literal
// from the sidecar .cpp); the registry stores it as a string_view.
struct FieldDesc {
    std::string_view name;
    TypeId           type        = TypeId::Unknown;
    uint32_t         offset      = 0;
    FieldMeta        meta        = {};
    uint32_t         enumTypeId  = 0;   // 0 = not an enum field; else registry id of E

    template <class T>
    T* ptr(void* obj) const {
        return reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }
    template <class T>
    const T* ptr(const void* obj) const {
        return reinterpret_cast<const T*>(
            static_cast<const uint8_t*>(obj) + offset);
    }
};
```

- [ ] **Step 5: Update `TypeBuilder::field` in `Reflection.h` to populate `enumTypeId`**

Edit `engine/reflection/Reflection.h`. Find the `TypeBuilder::field` body (lines ~28–41). Change the `push_back` line so the `FieldDesc` initializer carries `enumTypeIdOf<F>()`:

Old:
```cpp
            reg_.types_[typeId_].fields.push_back(
                FieldDesc{ name, TypeIdOf<F>::v, off, meta });
```

New:
```cpp
            reg_.types_[typeId_].fields.push_back(
                FieldDesc{ name, TypeIdOf<F>::v, off, meta, enumTypeIdOf<F>() });
```

- [ ] **Step 6: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all reflection tests pass.

- [ ] **Step 7: Commit**

```bash
git add engine/reflection/FieldDesc.h engine/reflection/TypeIdOf.h engine/reflection/Reflection.h tests/test_type_reflection.cpp
git commit -m "M39: FieldDesc.enumTypeId + TypeIdOf enum-id helpers"
```

---

### Task A3: `EnumBuilder` + `Reflection::registerEnum` / `enumValues` / `enumValuesById`

**Files:**
- Modify: `engine/reflection/Reflection.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
static void test_register_enum_stores_name() {
    iron::Reflection r;
    r.registerEnum<iron::PrimitiveKind>("PrimitiveKind")
        .value("cube",  iron::PrimitiveKind::Cube)
        .value("plane", iron::PrimitiveKind::Plane);
    CHECK(r.enumName<iron::PrimitiveKind>() == "PrimitiveKind");
}

static void test_unregistered_enum_has_empty_name() {
    iron::Reflection r;
    CHECK(r.enumName<iron::PrimitiveKind>().empty());
}

static void test_enum_values_lookup_template_and_by_id() {
    iron::Reflection r;
    r.registerEnum<iron::PrimitiveKind>("PrimitiveKind")
        .value("cube",  iron::PrimitiveKind::Cube)
        .value("plane", iron::PrimitiveKind::Plane);

    auto vs = r.enumValues<iron::PrimitiveKind>();
    CHECK(vs.size() == 2);
    CHECK(vs[0].name == "cube");
    CHECK(vs[1].name == "plane");
    CHECK(static_cast<iron::PrimitiveKind>(vs[0].value) == iron::PrimitiveKind::Cube);
    CHECK(static_cast<iron::PrimitiveKind>(vs[1].value) == iron::PrimitiveKind::Plane);

    const uint32_t id = iron::componentTypeId<iron::PrimitiveKind>();
    auto byId = r.enumValuesById(id);
    CHECK(byId.size() == 2);
    CHECK(byId[0].name == "cube");
    CHECK(r.enumNameById(id) == "PrimitiveKind");
}

static void test_unregistered_enum_id_returns_empty() {
    iron::Reflection r;
    const uint32_t id = iron::componentTypeId<iron::PrimitiveKind>();
    CHECK(r.enumValuesById(id).empty());
    CHECK(r.enumNameById(id).empty());
}
```

Register in `main()`:

```cpp
    test_register_enum_stores_name();
    test_unregistered_enum_has_empty_name();
    test_enum_values_lookup_template_and_by_id();
    test_unregistered_enum_id_returns_empty();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `registerEnum` / `enumValues` / `enumValuesById` / `enumNameById` / `enumName` not declared.

- [ ] **Step 3: Extend `Reflection.h` with the enum registry**

Edit `engine/reflection/Reflection.h`. Inside `class Reflection`, after the existing `fieldByName<T>` template (around line 86, before `private:`), append the public surface:

```cpp
    // ── Enum registry ──────────────────────────────────────────────────────
    struct EnumValue {
        std::string_view name;
        int64_t          value;
    };

    template <class E>
    class EnumBuilder {
    public:
        EnumBuilder& value(std::string_view name, E v) {
            static_assert(std::is_enum_v<E>, "EnumBuilder requires an enum type");
            reg_.enums_[typeId_].values.push_back(
                EnumValue{ name, static_cast<int64_t>(
                    static_cast<std::underlying_type_t<E>>(v)) });
            return *this;
        }
    private:
        friend class Reflection;
        EnumBuilder(Reflection& r, uint32_t id) : reg_(r), typeId_(id) {}
        Reflection& reg_;
        uint32_t    typeId_;
    };

    template <class E>
    EnumBuilder<E> registerEnum(std::string_view name) {
        static_assert(std::is_enum_v<E>, "registerEnum requires an enum type");
        const uint32_t id = componentTypeId<E>();
        assert(id < kMaxTypes && "Too many reflected enums (raise Reflection::kMaxTypes)");
        enums_[id].name = name;
        enums_[id].values.clear();
        enumsRegistered_[id] = true;
        return EnumBuilder<E>(*this, id);
    }

    template <class E>
    std::span<const EnumValue> enumValues() const {
        return enumValuesById(componentTypeId<E>());
    }

    template <class E>
    std::string_view enumName() const {
        return enumNameById(componentTypeId<E>());
    }

    // Dispatch-side, non-template lookups (Inspector / SceneIO call these via
    // FieldDesc::enumTypeId without knowing the concrete enum type E).
    std::span<const EnumValue> enumValuesById(uint32_t id) const {
        return (id < kMaxTypes && enumsRegistered_[id])
            ? std::span<const EnumValue>(enums_[id].values)
            : std::span<const EnumValue>{};
    }
    std::string_view enumNameById(uint32_t id) const {
        return (id < kMaxTypes && enumsRegistered_[id])
            ? enums_[id].name
            : std::string_view{};
    }
```

Then, at the bottom of the class (after the existing `TypeEntry types_` / `registered_` arrays), add the storage:

```cpp
    struct EnumEntry {
        std::string_view       name;
        std::vector<EnumValue> values;
    };
    std::array<EnumEntry, kMaxTypes> enums_{};
    std::array<bool, kMaxTypes>      enumsRegistered_{};
```

(`<type_traits>` for `std::underlying_type_t` and `std::is_enum_v` is already included at the top of the file.)

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all reflection tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/Reflection.h tests/test_type_reflection.cpp
git commit -m "M39: Reflection::registerEnum + EnumBuilder + enumValuesById"
```

---

## Phase B — `SceneEntity` contains `Transform` (mechanical refactor)

### Task B1: `SceneEntity` holds a `Transform`; rename `e.position` → `e.transform.position` across consumers

**Files:**
- Modify: `engine/scene/SceneFormat.h`
- Modify: `engine/scene/SceneIO.cpp`
- Modify: `engine/editor/Gizmo.cpp`
- Modify: `games/11-sandbox/main.cpp`
- Modify: `tests/test_scene_io.cpp`

The on-disk JSON format does NOT change in this task. SceneIO still emits flat top-level `position` / `rotation` / `scale` per entity — just through the new `e.transform.*` accessors. The format change to nested `"transform": {…}` lands in Task D1 alongside the demo.json migration.

`engine/editor/EnvironmentPanel.cpp` was grepped — its only `.position` / `.rotation` / `.scale` references are on `PointLight`, not `SceneEntity`. No edits needed there.

- [ ] **Step 1: Refactor `SceneEntity`**

Edit `engine/scene/SceneFormat.h`. Replace the `SceneEntity` struct (lines ~38–46):

```cpp
// One placed object: a transform + what to draw. The transform is M37's
// iron::Transform component (M39 unifies SceneEntity with the World's
// component model — see iron-core-engine-progress).
struct SceneEntity {
    std::string name;
    Transform   transform;
    MeshRef     mesh;
    MaterialDef material;
};
```

Add `#include "world/Transform.h"` near the existing `#include "math/Quaternion.h"` line at the top of the file.

The existing `#include "math/Vec.h"` and `#include "math/Quaternion.h"` can stay (no harm — `SceneEntity` no longer uses them, but `MaterialDef.emissive` and `MeshRef`/etc. transitively need them).

- [ ] **Step 2: Update `SceneIO.cpp` field accesses**

Edit `engine/scene/SceneIO.cpp`. In `entityToJson` (around lines 72–81), change three lines:

Old:
```cpp
    j["position"] = toJson(e.position);
    j["rotation"] = toJson(e.rotation);
    j["scale"]    = toJson(e.scale);
```

New:
```cpp
    j["position"] = toJson(e.transform.position);
    j["rotation"] = toJson(e.transform.rotation);
    j["scale"]    = toJson(e.transform.scale);
```

In `entityFromJson` (around lines 106–115), change three lines:

Old:
```cpp
    readVec3  (j, "position", e.position);
    readQuat  (j, "rotation", e.rotation);
    readVec3  (j, "scale",    e.scale);
```

New:
```cpp
    readVec3  (j, "position", e.transform.position);
    readQuat  (j, "rotation", e.transform.rotation);
    readVec3  (j, "scale",    e.transform.scale);
```

- [ ] **Step 3: Update `Gizmo.cpp` field accesses**

Edit `engine/editor/Gizmo.cpp`. There are 10 occurrences of `e.position` / `e.rotation` / `e.scale`. Use Edit with `replace_all` for the three patterns. Verify after each:

```
e.position  → e.transform.position
e.rotation  → e.transform.rotation
e.scale     → e.transform.scale
```

Caveats: `Gizmo::update`'s parameter `Vec3 origin` and the locals `startPos_` / `startScale_` / `startRot_` (members of `Gizmo`) are NOT renamed. Only `e.*` accesses. After the edit, grep to confirm no stray `e.position` remains in the file:

```bash
cmake --build build-vk --config Debug --target ironcore
```
Expected: clean build.

- [ ] **Step 4: Update `games/11-sandbox/main.cpp` field accesses**

Edit `games/11-sandbox/main.cpp`. Grep for the pattern `\b(e|se|ne|floor|helmet|cube[^.]*)\.(position|rotation|scale)\b` — there are about a dozen occurrences spread across:
- `resolveEntity` lambda (`e.position`, `e.rotation`, `e.scale`)
- The World-build pass that copies `se.position` etc. into `t.position` etc. — keep the right-hand side reads as `se.transform.position` etc.; the left-hand side `t.position` is on a `Transform` and stays
- The `appendAndSelect` lambda's World mirror (same shape)
- The selection-center function (`scene.entities[sel].position`)
- The per-frame re-derive `resolved[]` loop (re-reads `e.position` / `e.rotation` / `e.scale`)
- The end-of-frame Inspector → World sync (currently field-by-field copy — see Step 5 simplification)
- Each `iron::SceneEntity ne;` creation block (`ne.position = spawnPos();`)
- The `iron::SceneEntity ne = scene.entities[selectedIndex];` duplicate block (`ne.position.x += 0.5f;`)
- The post-update re-derive block in the gizmo handler
- The picking projection block (`se.position`)

All RHS `<entity>.position/rotation/scale` accesses on a `SceneEntity&` become `<entity>.transform.position` etc.

LHS assignments like `ne.position = spawnPos()` become `ne.transform.position = spawnPos()`.

Lines like `t.position = se.position;` where `t` is an `iron::Transform&` (from `world.get<Transform>(eid)`) keep `t.position` on the LHS but rewrite the RHS to `se.transform.position`. After Step 5 we collapse the three-line block into one assignment.

- [ ] **Step 5: Simplify the Inspector → World sync (optional polish, same task)**

Find the end-of-frame sync block (look for the comment "M37" or "Inspector → World" near line 755). The current block reads:

```cpp
            if (auto* t = world.tryGet<iron::Transform>(eid)) {
                t->position = se.position;
                t->rotation = se.rotation;
                t->scale    = se.scale;
            }
```

After the rename, it becomes:

```cpp
            if (auto* t = world.tryGet<iron::Transform>(eid)) {
                t->position = se.transform.position;
                t->rotation = se.transform.rotation;
                t->scale    = se.transform.scale;
            }
```

Then simplify to a single struct assignment:

```cpp
            if (auto* t = world.tryGet<iron::Transform>(eid)) {
                *t = se.transform;
            }
```

Apply the same simplification at the `appendAndSelect` lambda's initial World-mirror block, the World-build pass at scene-load time, and any other spot that does the three-line copy.

- [ ] **Step 6: Update `tests/test_scene_io.cpp`**

Edit `tests/test_scene_io.cpp`. In `makeSampleScene` (around lines 36–52), rename:

Old:
```cpp
    floor.position = {0.0f, 0.0f, 0.0f};
    floor.scale    = {20.0f, 1.0f, 20.0f};
```

New:
```cpp
    floor.transform.position = {0.0f, 0.0f, 0.0f};
    floor.transform.scale    = {20.0f, 1.0f, 20.0f};
```

Old:
```cpp
    helmet.position = {2.0f, 1.5f, 0.0f};
    helmet.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
    helmet.scale    = {1.5f, 1.5f, 1.5f};
```

New:
```cpp
    helmet.transform.position = {2.0f, 1.5f, 0.0f};
    helmet.transform.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
    helmet.transform.scale    = {1.5f, 1.5f, 1.5f};
```

In Test 1's assertions (around lines 85–96), rename:

Old:
```cpp
                CHECK_NEAR(l.entities[0].scale.x, 20.0f);
                ...
                const Quat q = l.entities[1].rotation;
```

New:
```cpp
                CHECK_NEAR(l.entities[0].transform.scale.x, 20.0f);
                ...
                const Quat q = l.entities[1].transform.rotation;
```

In Test 4's assertions (around lines 132–134), rename:

Old:
```cpp
                CHECK_NEAR(l.entities[0].position.x, 0.0f);
                CHECK_NEAR(l.entities[0].scale.x, 1.0f);
                CHECK_NEAR(l.entities[0].rotation.w, 1.0f);
```

New:
```cpp
                CHECK_NEAR(l.entities[0].transform.position.x, 0.0f);
                CHECK_NEAR(l.entities[0].transform.scale.x, 1.0f);
                CHECK_NEAR(l.entities[0].transform.rotation.w, 1.0f);
```

- [ ] **Step 7: Build + run full test suite**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build, 49/49 tests still green. demo.json still loads under the unchanged-on-disk format because SceneIO still emits/reads flat top-level position/rotation/scale.

- [ ] **Step 8: Commit**

```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.cpp engine/editor/Gizmo.cpp games/11-sandbox/main.cpp tests/test_scene_io.cpp
git commit -m "M39: SceneEntity contains Transform (mechanical rename across consumers)"
```

---

## Phase C — Reflection-driven helpers + sidecar widget hints

### Task C1: Sidecar updates — `MaterialDef` widget hints + `MeshRef` enum registration

Done first so the test in Task C2 can simply call `registerMeshRef(r)` (which transparently registers `PrimitiveKind`) and exercise the production wiring.

**Files:**
- Modify: `engine/scene/MeshRef.reflect.cpp`
- Modify: `engine/scene/MaterialDef.reflect.cpp`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
static void test_register_mesh_ref_also_registers_primitive_kind() {
    iron::Reflection r;
    iron::registerMeshRef(r);
    CHECK(r.enumName<iron::PrimitiveKind>() == "PrimitiveKind");
    auto vs = r.enumValues<iron::PrimitiveKind>();
    CHECK(vs.size() == 2);
    CHECK(vs[0].name == "cube");
    CHECK(static_cast<iron::PrimitiveKind>(vs[0].value) == iron::PrimitiveKind::Cube);
    CHECK(vs[1].name == "plane");
    CHECK(static_cast<iron::PrimitiveKind>(vs[1].value) == iron::PrimitiveKind::Plane);

    // MeshRef::primitive field carries the enum's typeId for dispatch.
    const iron::FieldDesc* p = r.fieldByName<iron::MeshRef>("primitive");
    CHECK(p != nullptr);
    CHECK(p->enumTypeId == iron::componentTypeId<iron::PrimitiveKind>());
}

static void test_material_def_widget_hints() {
    iron::Reflection r;
    iron::registerMaterialDef(r);
    const iron::FieldDesc* em = r.fieldByName<iron::MaterialDef>("emissive");
    CHECK(em != nullptr);
    CHECK(em->meta.color  == true);
    CHECK(em->meta.slider == false);

    const iron::FieldDesc* refl = r.fieldByName<iron::MaterialDef>("reflectivity");
    CHECK(refl != nullptr);
    CHECK(refl->meta.slider == true);
    CHECK(refl->meta.min    == 0.0f);
    CHECK(refl->meta.max    == 1.0f);
}
```

Register in `main()`:

```cpp
    test_register_mesh_ref_also_registers_primitive_kind();
    test_material_def_widget_hints();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: failures — `PrimitiveKind` enum is not registered, `emissive.color` is false, `reflectivity.slider` is false.

- [ ] **Step 3: Add enum registration to `MeshRef.reflect.cpp`**

Edit `engine/scene/MeshRef.reflect.cpp`. Replace the whole `registerMeshRef` body:

```cpp
#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMeshRef(Reflection& r) {
    r.registerEnum<PrimitiveKind>("PrimitiveKind")
        .value("cube",  PrimitiveKind::Cube)
        .value("plane", PrimitiveKind::Plane);
    r.registerType<MeshRef>("MeshRef")
        .field("primitive", &MeshRef::primitive)
        .field("gltfPath",  &MeshRef::gltfPath);
}

}  // namespace iron
```

- [ ] **Step 4: Add widget hints to `MaterialDef.reflect.cpp`**

Edit `engine/scene/MaterialDef.reflect.cpp`. Replace the whole `registerMaterialDef` body:

```cpp
#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMaterialDef(Reflection& r) {
    r.registerType<MaterialDef>("MaterialDef")
        .field("albedoPath",   &MaterialDef::albedoPath)
        .field("normalPath",   &MaterialDef::normalPath)
        .field("specularPath", &MaterialDef::specularPath)
        .field("emissive",     &MaterialDef::emissive,     {.color = true})
        .field("uvScale",      &MaterialDef::uvScale,      {.min = 0.0f, .max = 100.0f})
        .field("reflectivity", &MaterialDef::reflectivity, {.min = 0.0f, .max = 1.0f, .slider = true});
}

}  // namespace iron
```

- [ ] **Step 5: Build + run tests**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add engine/scene/MeshRef.reflect.cpp engine/scene/MaterialDef.reflect.cpp tests/test_type_reflection.cpp
git commit -m "M39: sidecar updates — PrimitiveKind enum + MaterialDef widget hints"
```

---

### Task C2: `engine/scene/ReflectionIO.{h,cpp}` + `tests/test_reflection_io.cpp`

**Files:**
- Create: `engine/scene/ReflectionIO.h`
- Create: `engine/scene/ReflectionIO.cpp`
- Create: `tests/test_reflection_io.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_reflection_io.cpp`:

```cpp
#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
#include "scene/ReflectionIO.h"
#include "scene/SceneFormat.h"
#include "world/Transform.h"

#include <nlohmann/json.hpp>

#include <cstdio>

using nlohmann::json;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static iron::Reflection makeReg() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    return r;
}

static void test_transform_roundtrip() {
    iron::Reflection r = makeReg();
    iron::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = iron::Quat::fromAxisAngle(iron::Vec3{0, 1, 0}, 0.5f);
    t.scale    = {1.5f, 2.5f, 3.5f};

    json j = iron::componentToJson(r, t);
    iron::Transform back;
    iron::componentFromJson(r, back, j);

    CHECK(back.position.x == 1.0f);
    CHECK(back.position.y == 2.0f);
    CHECK(back.position.z == 3.0f);
    CHECK(back.scale.x    == 1.5f);
    CHECK(back.rotation.w == t.rotation.w);
}

static void test_mesh_ref_roundtrip_primitive_cube() {
    iron::Reflection r = makeReg();
    iron::MeshRef m;
    m.primitive = iron::PrimitiveKind::Cube;
    json j = iron::componentToJson(r, m);
    CHECK(j.contains("primitive"));
    CHECK(j["primitive"].get<std::string>() == "cube");

    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.primitive.has_value());
    CHECK(back.primitive.value() == iron::PrimitiveKind::Cube);
}

static void test_mesh_ref_roundtrip_primitive_none_omits_key() {
    iron::Reflection r = makeReg();
    iron::MeshRef m;
    m.primitive = std::nullopt;
    m.gltfPath  = "assets/foo.gltf";
    json j = iron::componentToJson(r, m);
    CHECK(!j.contains("primitive"));     // nullopt → omitted
    CHECK(j["gltfPath"].get<std::string>() == "assets/foo.gltf");

    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    CHECK(!back.primitive.has_value());
    CHECK(back.gltfPath == "assets/foo.gltf");
}

static void test_material_def_roundtrip_full() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;
    m.albedoPath   = "a.png";
    m.normalPath   = "n.png";
    m.specularPath = "s.png";
    m.emissive     = {0.4f, 0.5f, 0.6f};
    m.uvScale      = 2.0f;
    m.reflectivity = 0.7f;
    json j = iron::componentToJson(r, m);

    iron::MaterialDef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.albedoPath   == "a.png");
    CHECK(back.normalPath   == "n.png");
    CHECK(back.specularPath == "s.png");
    CHECK(back.emissive.x   == 0.4f);
    CHECK(back.uvScale      == 2.0f);
    CHECK(back.reflectivity == 0.7f);
}

static void test_material_def_empty_strings_omitted() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;                        // all path fields empty
    m.emissive = {0.1f, 0.2f, 0.3f};
    json j = iron::componentToJson(r, m);
    CHECK(!j.contains("albedoPath"));
    CHECK(!j.contains("normalPath"));
    CHECK(!j.contains("specularPath"));
    CHECK(j["emissive"].is_array());
}

static void test_unknown_enum_name_logs_falls_back_to_first_value() {
    iron::Reflection r = makeReg();
    json j = {{"primitive", "bogus"}};
    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    // Spec: unknown enum string → first registered value (cube).
    CHECK(back.primitive.has_value());
    CHECK(back.primitive.value() == iron::PrimitiveKind::Cube);
}

static void test_widget_hints_do_not_affect_serialization() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;
    m.emissive     = {0.3f, 0.3f, 0.3f};
    m.reflectivity = 0.5f;
    json j = iron::componentToJson(r, m);
    // emissive is a 3-element array (NOT a hex string, NOT an object);
    // reflectivity is a number (NOT a slider widget marker).
    CHECK(j["emissive"].is_array());
    CHECK(j["emissive"].size() == 3u);
    CHECK(j["reflectivity"].is_number());
}

static void test_min_max_do_not_clamp_on_load() {
    iron::Reflection r = makeReg();
    json j = json::object();
    j["reflectivity"] = 1.5f;   // out of [0, 1]
    iron::MaterialDef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.reflectivity == 1.5f);   // stored as-is; UI clamps, IO does not
}

int main() {
    test_transform_roundtrip();
    test_mesh_ref_roundtrip_primitive_cube();
    test_mesh_ref_roundtrip_primitive_none_omits_key();
    test_material_def_roundtrip_full();
    test_material_def_empty_strings_omitted();
    test_unknown_enum_name_logs_falls_back_to_first_value();
    test_widget_hints_do_not_affect_serialization();
    test_min_max_do_not_clamp_on_load();
    if (g_failures == 0) std::printf("All reflection-IO tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
```

Add registration to `tests/CMakeLists.txt` after the existing `iron_add_test(test_type_reflection …)` line:

```cmake
iron_add_test(test_reflection_io test_reflection_io.cpp)
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_reflection_io
```
Expected: compile error — `scene/ReflectionIO.h` does not exist.

- [ ] **Step 3: Create `engine/scene/ReflectionIO.h`**

```cpp
#pragma once

#include "reflection/Reflection.h"

#include <nlohmann/json.hpp>

// Generic component ↔ JSON helpers driven by the iron::Reflection registry.
// Walks fieldsOf<T>() and dispatches per TypeId. Empty strings are omitted
// on write; nullopt enums are omitted entirely. Widget hints (color, slider,
// dragSpeed, min/max) DO NOT affect serialization.
//
// The Reflection& parameter is required so the dispatch can resolve enum
// value names via enumValuesById(FieldDesc::enumTypeId).

namespace iron {

template <class T>
nlohmann::json componentToJson(const Reflection& r, const T& obj);

template <class T>
void componentFromJson(const Reflection& r, T& obj, const nlohmann::json& j);

// Non-template byte-pointer workers — public so the templates above can be
// defined inline here without pulling the implementation into every TU.
nlohmann::json componentToJsonByPtr(const Reflection& r,
                                    std::span<const FieldDesc> fields,
                                    const void* obj);
void           componentFromJsonByPtr(const Reflection& r,
                                      std::span<const FieldDesc> fields,
                                      void* obj,
                                      const nlohmann::json& j);

template <class T>
nlohmann::json componentToJson(const Reflection& r, const T& obj) {
    return componentToJsonByPtr(r, r.fieldsOf<T>(), &obj);
}

template <class T>
void componentFromJson(const Reflection& r, T& obj, const nlohmann::json& j) {
    componentFromJsonByPtr(r, r.fieldsOf<T>(), &obj, j);
}

}  // namespace iron
```

- [ ] **Step 4: Create `engine/scene/ReflectionIO.cpp`**

```cpp
#include "scene/ReflectionIO.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cstdint>
#include <string>

namespace iron {

namespace {

using json = nlohmann::json;

json vec3ToJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json quatToJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

void readVec3(const json& j, Vec3& out) {
    if (j.is_array() && j.size() == 3) {
        out.x = j[0].get<float>();
        out.y = j[1].get<float>();
        out.z = j[2].get<float>();
    }
}
void readQuat(const json& j, Quat& out) {
    if (j.is_array() && j.size() == 4) {
        out.x = j[0].get<float>();
        out.y = j[1].get<float>();
        out.z = j[2].get<float>();
        out.w = j[3].get<float>();
    }
}

// Look up an EnumValue by integer (round-tripping through int64_t) — used to
// serialize the current enum value as its registered name.
std::string_view enumValueName(std::span<const Reflection::EnumValue> values,
                               int64_t v) {
    for (const auto& ev : values)
        if (ev.value == v) return ev.name;
    return {};
}

}  // namespace

json componentToJsonByPtr(const Reflection& r,
                          std::span<const FieldDesc> fields,
                          const void* obj) {
    json out = json::object();
    for (const FieldDesc& f : fields) {
        switch (f.type) {
            case TypeId::Bool:
                out[std::string(f.name)] = *f.ptr<bool>(obj);
                break;
            case TypeId::Int32:
                out[std::string(f.name)] = *f.ptr<int32_t>(obj);
                break;
            case TypeId::UInt32:
                out[std::string(f.name)] = *f.ptr<uint32_t>(obj);
                break;
            case TypeId::UInt8:
                out[std::string(f.name)] = *f.ptr<uint8_t>(obj);
                break;
            case TypeId::Float:
                out[std::string(f.name)] = *f.ptr<float>(obj);
                break;
            case TypeId::String: {
                const std::string& s = *f.ptr<std::string>(obj);
                if (!s.empty()) out[std::string(f.name)] = s;  // omit empties
                break;
            }
            case TypeId::Vec3:
                out[std::string(f.name)] = vec3ToJson(*f.ptr<Vec3>(obj));
                break;
            case TypeId::Quat:
                out[std::string(f.name)] = quatToJson(*f.ptr<Quat>(obj));
                break;
            case TypeId::Enum: {
                auto vs = r.enumValuesById(f.enumTypeId);
                // Read enum value as its underlying int64 (assumes enum's
                // underlying type fits — true for all our enums).
                const int64_t v = static_cast<int64_t>(
                    *reinterpret_cast<const int32_t*>(
                        static_cast<const uint8_t*>(obj) + f.offset));
                const std::string_view name = enumValueName(vs, v);
                if (!name.empty())
                    out[std::string(f.name)] = std::string(name);
                else
                    Log::warn("ReflectionIO: enum '%.*s' value %lld has no registered name; omitting",
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<long long>(v));
                break;
            }
            case TypeId::OptionalEnum: {
                // Treat the optional<E> like its underlying enum payload for
                // serialization purposes (omit when not set).
                const auto& opt = *f.ptr<std::optional<int32_t>>(obj);
                if (opt.has_value()) {
                    auto vs = r.enumValuesById(f.enumTypeId);
                    const std::string_view name = enumValueName(
                        vs, static_cast<int64_t>(*opt));
                    if (!name.empty())
                        out[std::string(f.name)] = std::string(name);
                }
                break;
            }
            case TypeId::Unknown:
                Log::warn("ReflectionIO: field '%.*s' has Unknown TypeId; skipped",
                          static_cast<int>(f.name.size()), f.name.data());
                break;
        }
    }
    return out;
}

void componentFromJsonByPtr(const Reflection& r,
                            std::span<const FieldDesc> fields,
                            void* obj,
                            const json& j) {
    if (!j.is_object()) return;
    for (const FieldDesc& f : fields) {
        const std::string key(f.name);
        const bool present = j.contains(key);
        switch (f.type) {
            case TypeId::Bool:
                if (present && j[key].is_boolean()) *f.ptr<bool>(obj) = j[key].get<bool>();
                break;
            case TypeId::Int32:
                if (present && j[key].is_number_integer()) *f.ptr<int32_t>(obj) = j[key].get<int32_t>();
                break;
            case TypeId::UInt32:
                if (present && j[key].is_number_integer()) *f.ptr<uint32_t>(obj) = j[key].get<uint32_t>();
                break;
            case TypeId::UInt8:
                if (present && j[key].is_number_integer()) *f.ptr<uint8_t>(obj) = static_cast<uint8_t>(j[key].get<uint32_t>());
                break;
            case TypeId::Float:
                if (present && j[key].is_number()) *f.ptr<float>(obj) = j[key].get<float>();
                break;
            case TypeId::String:
                if (present && j[key].is_string()) *f.ptr<std::string>(obj) = j[key].get<std::string>();
                break;
            case TypeId::Vec3:
                if (present) readVec3(j[key], *f.ptr<Vec3>(obj));
                break;
            case TypeId::Quat:
                if (present) readQuat(j[key], *f.ptr<Quat>(obj));
                break;
            case TypeId::Enum: {
                if (!present || !j[key].is_string()) break;
                auto vs = r.enumValuesById(f.enumTypeId);
                const std::string s = j[key].get<std::string>();
                int64_t matched = vs.empty() ? 0 : vs[0].value;  // fallback: first
                bool found = false;
                for (const auto& ev : vs) {
                    if (ev.name == s) { matched = ev.value; found = true; break; }
                }
                if (!found && !vs.empty()) {
                    Log::warn("ReflectionIO: unknown enum value '%s' for field '%.*s'; defaulting to '%.*s'",
                              s.c_str(),
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<int>(vs[0].name.size()), vs[0].name.data());
                }
                // Underlying type is int32 for both our enums — safe today.
                *reinterpret_cast<int32_t*>(
                    static_cast<uint8_t*>(obj) + f.offset) = static_cast<int32_t>(matched);
                break;
            }
            case TypeId::OptionalEnum: {
                if (!present) {
                    f.ptr<std::optional<int32_t>>(obj)->reset();
                    break;
                }
                if (!j[key].is_string()) break;
                auto vs = r.enumValuesById(f.enumTypeId);
                const std::string s = j[key].get<std::string>();
                int64_t matched = vs.empty() ? 0 : vs[0].value;
                bool found = false;
                for (const auto& ev : vs) {
                    if (ev.name == s) { matched = ev.value; found = true; break; }
                }
                if (!found && !vs.empty()) {
                    Log::warn("ReflectionIO: unknown enum value '%s' for field '%.*s'; defaulting to '%.*s'",
                              s.c_str(),
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<int>(vs[0].name.size()), vs[0].name.data());
                }
                *f.ptr<std::optional<int32_t>>(obj) = static_cast<int32_t>(matched);
                break;
            }
            case TypeId::Unknown:
                break;
        }
    }
}

}  // namespace iron
```

Note on the enum dispatch's reliance on int32 underlying type: today both `PrimitiveKind` (default-typed enum class → `int`) and any future enums declared without an explicit underlying type fit this. If an enum is declared with a different underlying type (e.g. `enum class X : uint8_t`), the cast at `*reinterpret_cast<int32_t*>(…)` would access too many bytes — flag this as a known v1 limit in the PR and add a `static_assert` in `EnumBuilder::value` if a future enum trips on it.

- [ ] **Step 5: Register the new source file in `engine/CMakeLists.txt`**

Edit `engine/CMakeLists.txt`. Inside the `add_library(ironcore STATIC ...)` list, append after the four `.reflect.cpp` lines:

```cmake
  scene/ReflectionIO.cpp
```

(Keep the alphabetical-ish grouping near `scene/SceneIO.cpp` is also fine.)

- [ ] **Step 6: Build + run all tests**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_reflection_io
cd build-vk && ctest -C Debug --output-on-failure
```
Expected: clean build; 50/50 tests pass.

- [ ] **Step 7: Commit**

```bash
git add engine/scene/ReflectionIO.h engine/scene/ReflectionIO.cpp engine/CMakeLists.txt tests/CMakeLists.txt tests/test_reflection_io.cpp
git commit -m "M39: ReflectionIO — componentToJson / componentFromJson + tests"
```

---

### Task C3: `engine/editor/ReflectionInspector.{h,cpp}`

**Files:**
- Create: `engine/editor/ReflectionInspector.h`
- Create: `engine/editor/ReflectionInspector.cpp`
- Modify: `engine/CMakeLists.txt`

No new unit tests — the dispatch is exercised by the sandbox visual gate in Task E1. ImGui isn't unit-testable here.

- [ ] **Step 1: Create `engine/editor/ReflectionInspector.h`**

```cpp
#pragma once

#include "reflection/Reflection.h"

// ImGui-rendered Inspector helper driven by the iron::Reflection registry.
// Walks fieldsOf<T>(), draws one widget per field by dispatching on TypeId,
// returns true if any field changed this frame. The component's type name
// (typeName<T>()) is used as the section header (ImGui::SeparatorText).
//
// Widget rules per TypeId:
//   Bool         → Checkbox
//   Int32/UInt32 → InputInt (clamped if min/max set)
//   UInt8        → InputInt (clamped to [0, 255] regardless of meta)
//   Float        → SliderFloat if meta.slider, else DragFloat (speed from
//                  meta.dragSpeed or 0.05; clamped if min/max set)
//   String       → InputText (size grows as user types)
//   Vec3         → ColorEdit3 if meta.color, else DragFloat3 (speed from
//                  meta.dragSpeed or 0.05)
//   Quat         → euler-decomposed DragFloat3 (degrees); recompose on edit
//                  (speed = meta.dragSpeed or 0.5)
//   Enum         → Combo over enumValuesById(f.enumTypeId); unknown current
//                  value shows index 0 with a one-shot warning and does not
//                  write back until the user picks a value
//   OptionalEnum → "None" + Combo; None resets the optional, others assign
//
// ImGui include is kept private to ReflectionInspector.cpp — consumers of the
// header don't pull <imgui.h>.

namespace iron {

template <class T>
bool renderComponent(const Reflection& r, T& obj);

// Non-template byte-pointer worker; the template above just forwards.
bool renderComponentByPtr(const Reflection& r,
                          std::string_view typeName,
                          std::span<const FieldDesc> fields,
                          void* obj);

template <class T>
bool renderComponent(const Reflection& r, T& obj) {
    return renderComponentByPtr(r, r.typeName<T>(), r.fieldsOf<T>(), &obj);
}

}  // namespace iron
```

- [ ] **Step 2: Create `engine/editor/ReflectionInspector.cpp`**

```cpp
#include "editor/ReflectionInspector.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>

namespace iron {

namespace {

constexpr float kVec3DefaultSpeed  = 0.05f;
constexpr float kFloatDefaultSpeed = 0.05f;
constexpr float kEulerDefaultSpeed = 0.5f;

bool drawString(const FieldDesc& f, void* obj) {
    auto* s = f.ptr<std::string>(obj);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s->c_str());
    if (ImGui::InputText(std::string(f.name).c_str(), buf, sizeof(buf))) {
        *s = buf;
        return true;
    }
    return false;
}

bool drawFloat(const FieldDesc& f, void* obj) {
    auto* v = f.ptr<float>(obj);
    const std::string label(f.name);   // keep alive for the call below
    if (f.meta.slider && (f.meta.min != f.meta.max))
        return ImGui::SliderFloat(label.c_str(), v, f.meta.min, f.meta.max);
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kFloatDefaultSpeed;
    return ImGui::DragFloat(label.c_str(), v, speed, f.meta.min, f.meta.max);
}

bool drawVec3(const FieldDesc& f, void* obj) {
    auto* v = f.ptr<Vec3>(obj);
    const std::string label(f.name);
    if (f.meta.color)
        return ImGui::ColorEdit3(label.c_str(), &v->x);
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kVec3DefaultSpeed;
    return ImGui::DragFloat3(label.c_str(), &v->x, speed, f.meta.min, f.meta.max);
}

bool drawQuat(const FieldDesc& f, void* obj) {
    auto* q = f.ptr<Quat>(obj);
    Vec3 euler = quatToEuler(*q);   // degrees
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kEulerDefaultSpeed;
    if (ImGui::DragFloat3(std::string(f.name).c_str(), &euler.x, speed)) {
        *q = eulerToQuat(euler);
        return true;
    }
    return false;
}

bool drawBool(const FieldDesc& f, void* obj) {
    return ImGui::Checkbox(std::string(f.name).c_str(), f.ptr<bool>(obj));
}

bool drawIntLike(const FieldDesc& f, void* obj) {
    // Read via int32 (signed-min for UInt32/UInt8 cases is OK for InputInt's
    // user-facing widget; assignment narrows at the end).
    const std::string label(f.name);
    int32_t v = 0;
    switch (f.type) {
        case TypeId::Int32:  v = *f.ptr<int32_t>(obj); break;
        case TypeId::UInt32: v = static_cast<int32_t>(*f.ptr<uint32_t>(obj)); break;
        case TypeId::UInt8:  v = static_cast<int32_t>(*f.ptr<uint8_t>(obj));  break;
        default: break;
    }
    if (!ImGui::InputInt(label.c_str(), &v)) return false;
    if (f.type == TypeId::UInt8) {
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
    } else if (f.meta.min != f.meta.max) {
        if (v < static_cast<int32_t>(f.meta.min)) v = static_cast<int32_t>(f.meta.min);
        if (v > static_cast<int32_t>(f.meta.max)) v = static_cast<int32_t>(f.meta.max);
    }
    switch (f.type) {
        case TypeId::Int32:  *f.ptr<int32_t>(obj)  = v; break;
        case TypeId::UInt32: *f.ptr<uint32_t>(obj) = static_cast<uint32_t>(v); break;
        case TypeId::UInt8:  *f.ptr<uint8_t>(obj)  = static_cast<uint8_t>(v);  break;
        default: break;
    }
    return true;
}

// Build a flat null-terminated buffer of "name1\0name2\0..." for ImGui::Combo.
// Returned vector owns the chars; the const char* alias is stable for the
// duration of the same frame, which is all ImGui needs.
struct ComboNames {
    std::vector<char> chars;
};

ComboNames buildComboNames(std::span<const Reflection::EnumValue> values,
                           bool prependNone) {
    ComboNames out;
    if (prependNone) {
        out.chars.insert(out.chars.end(), {'N', 'o', 'n', 'e', '\0'});
    }
    for (const auto& v : values) {
        out.chars.insert(out.chars.end(), v.name.begin(), v.name.end());
        out.chars.push_back('\0');
    }
    out.chars.push_back('\0');  // double-null terminator
    return out;
}

bool drawEnum(const Reflection& r, const FieldDesc& f, void* obj) {
    auto values = r.enumValuesById(f.enumTypeId);
    if (values.empty()) {
        ImGui::TextDisabled("%s (no enum registered)", std::string(f.name).c_str());
        return false;
    }
    const int32_t cur = *reinterpret_cast<int32_t*>(
        static_cast<uint8_t*>(obj) + f.offset);
    int picked = -1;
    for (size_t i = 0; i < values.size(); ++i) {
        if (static_cast<int32_t>(values[i].value) == cur) { picked = static_cast<int>(i); break; }
    }
    if (picked < 0) {
        Log::warn("ReflectionInspector: enum field '%.*s' has unregistered value %d; showing index 0",
                  static_cast<int>(f.name.size()), f.name.data(),
                  static_cast<int>(cur));
        picked = 0;
    }
    ComboNames names = buildComboNames(values, /*prependNone=*/false);
    if (!ImGui::Combo(std::string(f.name).c_str(), &picked, names.chars.data())) return false;
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) + f.offset) =
        static_cast<int32_t>(values[picked].value);
    return true;
}

bool drawOptionalEnum(const Reflection& r, const FieldDesc& f, void* obj) {
    auto values = r.enumValuesById(f.enumTypeId);
    auto* opt = f.ptr<std::optional<int32_t>>(obj);
    int picked = 0;   // index 0 = "None"
    if (opt->has_value()) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (static_cast<int32_t>(values[i].value) == *opt) {
                picked = static_cast<int>(i) + 1;   // +1 for "None"
                break;
            }
        }
    }
    ComboNames names = buildComboNames(values, /*prependNone=*/true);
    if (!ImGui::Combo(std::string(f.name).c_str(), &picked, names.chars.data())) return false;
    if (picked == 0) {
        opt->reset();
    } else {
        *opt = static_cast<int32_t>(values[picked - 1].value);
    }
    return true;
}

}  // namespace

bool renderComponentByPtr(const Reflection& r,
                          std::string_view typeName,
                          std::span<const FieldDesc> fields,
                          void* obj) {
    if (!typeName.empty()) ImGui::SeparatorText(std::string(typeName).c_str());
    bool changed = false;
    for (const FieldDesc& f : fields) {
        switch (f.type) {
            case TypeId::Bool:   changed |= drawBool(f, obj);          break;
            case TypeId::Int32:
            case TypeId::UInt32:
            case TypeId::UInt8:  changed |= drawIntLike(f, obj);       break;
            case TypeId::Float:  changed |= drawFloat(f, obj);         break;
            case TypeId::String: changed |= drawString(f, obj);        break;
            case TypeId::Vec3:   changed |= drawVec3(f, obj);          break;
            case TypeId::Quat:   changed |= drawQuat(f, obj);          break;
            case TypeId::Enum:   changed |= drawEnum(r, f, obj);       break;
            case TypeId::OptionalEnum:
                                 changed |= drawOptionalEnum(r, f, obj); break;
            case TypeId::Unknown:
                ImGui::TextDisabled("%s (unknown type)", std::string(f.name).c_str());
                break;
        }
    }
    return changed;
}

}  // namespace iron
```

- [ ] **Step 3: Register the new source file in `engine/editor/CMakeLists.txt`**

`ReflectionInspector.cpp` belongs in the editor module (the file uses ImGui, which is PRIVATE-linked to `ironcore_editor` and NOT to `ironcore`). Edit `engine/editor/CMakeLists.txt` and append to the `add_library(ironcore_editor STATIC ...)` source list:

```cmake
  ReflectionInspector.cpp
```

After the edit, that list reads:
```cmake
add_library(ironcore_editor STATIC
  Gizmo.cpp
  ImGuiLayer.cpp
  SceneOutliner.cpp
  SceneInspector.cpp
  EnvironmentPanel.cpp
  ReflectionInspector.cpp
)
```

Do NOT add it to `engine/CMakeLists.txt`'s `ironcore` source list — that would force ImGui to leak into every shipping-game build.

- [ ] **Step 4: Build**

```bash
cmake --build build-vk --config Debug --target ironcore
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; 50/50 tests still green (no new tests, dispatch is visual-gated).

- [ ] **Step 5: Commit**

```bash
git add engine/editor/ReflectionInspector.h engine/editor/ReflectionInspector.cpp engine/CMakeLists.txt
git commit -m "M39: ReflectionInspector — renderComponent<T> + ImGui dispatch"
```

---

## Phase D — Wire-up

### Task D1: `SceneInspector` body, `SceneIO` entity ser/deser, sandbox host, `demo.json` migration

**Files:**
- Modify: `engine/editor/SceneInspector.h`
- Modify: `engine/editor/SceneInspector.cpp`
- Modify: `engine/scene/SceneIO.h`
- Modify: `engine/scene/SceneIO.cpp`
- Modify: `games/11-sandbox/main.cpp`
- Modify: `games/11-sandbox/assets/scenes/demo.json`
- Modify: `tests/test_scene_io.cpp`

- [ ] **Step 1: Update `SceneInspector` to take `const Reflection&`**

Edit `engine/editor/SceneInspector.h`:

```cpp
#pragma once

#include "editor/Gizmo.h"      // for GizmoSpace
#include "render/PostEffect.h" // for EffectKind

namespace iron {

struct SceneEntity;
class  Reflection;

// Details panel for a single entity. Renders transform / mesh / material via
// iron::Reflection-driven dispatch. Editor-tool widgets (gizmo space, effect
// picker) stay hand-rolled — they are not entity data.
class SceneInspector {
public:
    // Returns true if any entity field changed this frame.
    bool draw(const Reflection& reflection,
              SceneEntity& entity,
              GizmoSpace& space,
              EffectKind& effectKind);
};

}  // namespace iron
```

Edit `engine/editor/SceneInspector.cpp` — replace the entire body:

```cpp
#include "editor/SceneInspector.h"

#include "editor/ReflectionInspector.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneInspector::draw(const Reflection& reflection,
                          SceneEntity& e,
                          GizmoSpace& space,
                          EffectKind& effectKind) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Gizmo space toggle (mirrors the X key). Scale handles are always local
    // regardless of this setting. Editor-tool state — not entity data, stays
    // hand-rolled.
    ImGui::SeparatorText("Gizmo Space");
    int spaceInt = (space == GizmoSpace::Local) ? 1 : 0;
    bool spaceChanged = false;
    spaceChanged |= ImGui::RadioButton("World", &spaceInt, 0);
    ImGui::SameLine();
    spaceChanged |= ImGui::RadioButton("Local", &spaceInt, 1);
    if (spaceChanged)
        space = (spaceInt == 1) ? GizmoSpace::Local : GizmoSpace::World;

    // Selection effect picker — editor tool state, not entity data.
    ImGui::SeparatorText("Selection Effect");
    const char* kinds[] = {"None", "Outline", "Glowing Outline", "X-Ray"};
    int ki = static_cast<int>(effectKind);
    if (ImGui::Combo("Effect", &ki, kinds, 4))
        effectKind = static_cast<EffectKind>(ki);

    // Entity body — purely reflection-driven.
    changed |= renderComponent(reflection, e.transform);
    changed |= renderComponent(reflection, e.mesh);
    changed |= renderComponent(reflection, e.material);

    ImGui::End();
    return changed;
}

}  // namespace iron
```

- [ ] **Step 2: Update `SceneIO.h` to take `const Reflection&`**

Edit `engine/scene/SceneIO.h`:

```cpp
#pragma once

#include "scene/SceneFormat.h"

#include <optional>
#include <string>

namespace iron {

class Reflection;

// Load a scene from a JSON file. Returns nullopt on a missing file or
// malformed JSON (logs the error via Log::error). Missing optional fields
// fall back to the SceneFile / SceneEntity struct defaults. The Reflection
// registry is consulted for component (transform / mesh / material) fields
// and any enum registered in it.
std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const std::string& path);

// Write a scene to a JSON file (pretty-printed, 2-space indent, for
// human-diffable output). Returns false if the file can't be opened.
bool saveSceneFile(const Reflection& reflection,
                   const SceneFile& scene,
                   const std::string& path);

}  // namespace iron
```

- [ ] **Step 3: Rewrite `SceneIO.cpp` entity ser/deser to use `ReflectionIO`**

Edit `engine/scene/SceneIO.cpp`. Replace the entire entity-related ser/deser block (drop the `meshToJson` / `meshFromJson` / `materialToJson` / `materialFromJson` / `primitiveName` helpers — they're now dead code subsumed by `ReflectionIO`). The full new file:

```cpp
#include "scene/SceneIO.h"

#include "core/Log.h"
#include "scene/ReflectionIO.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace iron {

namespace {

using json = nlohmann::json;

json toJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

void readVec3(const json& j, const char* key, Vec3& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
    }
}

void readFloat(const json& j, const char* key, float& out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
}

void readString(const json& j, const char* key, std::string& out) {
    if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}

json entityToJson(const Reflection& r, const SceneEntity& e) {
    json j = json::object();
    j["name"]      = e.name;
    j["transform"] = componentToJson(r, e.transform);
    j["mesh"]      = componentToJson(r, e.mesh);
    j["material"]  = componentToJson(r, e.material);
    return j;
}

SceneEntity entityFromJson(const Reflection& r, const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    if (j.contains("transform")) componentFromJson(r, e.transform, j["transform"]);
    if (j.contains("mesh"))      componentFromJson(r, e.mesh,      j["mesh"]);
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);
    return e;
}

}  // namespace

bool saveSceneFile(const Reflection& reflection,
                   const SceneFile& scene,
                   const std::string& path) {
    json root = json::object();
    root["clearColor"] = toJson(scene.clearColor);

    json sun = json::object();
    sun["direction"] = toJson(scene.sun.direction);
    sun["color"]     = toJson(scene.sun.color);
    sun["ambient"]   = scene.sun.ambient;
    root["sun"] = sun;

    json fog = json::object();
    fog["color"]   = toJson(scene.fog.color);
    fog["density"] = scene.fog.density;
    root["fog"] = fog;

    json pls = json::array();
    for (const auto& pl : scene.pointLights) {
        json j = json::object();
        j["position"]  = toJson(pl.position);
        j["color"]     = toJson(pl.color);
        j["intensity"] = pl.intensity;
        j["range"]     = pl.range;
        pls.push_back(j);
    }
    root["pointLights"] = pls;

    json ents = json::array();
    for (const auto& e : scene.entities) ents.push_back(entityToJson(reflection, e));
    root["entities"] = ents;

    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);
    return true;
}

std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        Log::error("SceneIO: parse error in '%s': %s", path.c_str(), e.what());
        return std::nullopt;
    }

    SceneFile scene;
    readVec3(root, "clearColor", scene.clearColor);

    if (root.contains("sun")) {
        const json& sun = root["sun"];
        readVec3 (sun, "direction", scene.sun.direction);
        readVec3 (sun, "color",     scene.sun.color);
        readFloat(sun, "ambient",   scene.sun.ambient);
    }
    if (root.contains("fog")) {
        const json& fog = root["fog"];
        readVec3 (fog, "color",   scene.fog.color);
        readFloat(fog, "density", scene.fog.density);
    }
    if (root.contains("pointLights") && root["pointLights"].is_array()) {
        for (const auto& j : root["pointLights"]) {
            PointLight pl;
            readVec3 (j, "position",  pl.position);
            readVec3 (j, "color",     pl.color);
            readFloat(j, "intensity", pl.intensity);
            readFloat(j, "range",     pl.range);
            scene.pointLights.push_back(pl);
        }
    }
    if (root.contains("entities") && root["entities"].is_array()) {
        for (const auto& j : root["entities"]) {
            scene.entities.push_back(entityFromJson(reflection, j));
        }
    }
    return scene;
}

}  // namespace iron
```

- [ ] **Step 4: Migrate `demo.json` to the nested format**

Edit `games/11-sandbox/assets/scenes/demo.json`. Replace the contents:

```json
{
  "clearColor": [0.45, 0.55, 0.7],
  "sun": { "direction": [-0.5, -1.0, -0.3], "color": [1.0, 0.97, 0.9], "ambient": 0.2 },
  "fog": { "color": [0.45, 0.55, 0.7], "density": 0.0 },
  "pointLights": [
    { "position": [-2.0, 3.0, 2.0], "color": [1.0, 0.6, 0.3], "intensity": 2.5, "range": 14.0 }
  ],
  "entities": [
    {
      "name": "floor",
      "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [20, 1, 20] },
      "mesh": { "primitive": "plane" },
      "material": { "emissive": [0.05, 0.05, 0.06], "uvScale": 1.0 }
    },
    {
      "name": "cube-red",
      "transform": { "position": [-2, 1, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.6, 0.15, 0.1] }
    },
    {
      "name": "cube-green",
      "transform": { "position": [0, 1, -2], "rotation": [0, 0, 0, 1], "scale": [1, 2, 1] },
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.1, 0.5, 0.15] }
    },
    {
      "name": "helmet",
      "transform": { "position": [2.5, 1.5, 0], "rotation": [0, 0.3826834, 0, 0.9238795], "scale": [1.5, 1.5, 1.5] },
      "mesh": { "gltfPath": "assets/damaged-helmet/DamagedHelmet.gltf" },
      "material": { "reflectivity": 0.2 }
    }
  ]
}
```

- [ ] **Step 5: Update `games/11-sandbox/main.cpp` calls**

Find the load call (around line 288):

Old:
```cpp
    const auto sceneOpt = iron::loadSceneFile(scenePath);
```

New:
```cpp
    const auto sceneOpt = iron::loadSceneFile(reflection, scenePath);
```

Find the save call (around line 669, inside a save hotkey block):

Old:
```cpp
            if (iron::saveSceneFile(scene, scenePath))
```

New:
```cpp
            if (iron::saveSceneFile(reflection, scene, scenePath))
```

Find the inspector.draw call (around line 655):

Old:
```cpp
            inspector.draw(scene.entities[selectedIndex], sp, ek);
```

New:
```cpp
            inspector.draw(reflection, scene.entities[selectedIndex], sp, ek);
```

The load call comes BEFORE the reflection registry is constructed in the current source (line 288 vs line 314). Move the four reflection registration calls + the registry declaration above the `loadSceneFile` call, OR construct the registry earlier — easiest is to move the `iron::Reflection reflection;` + four `registerXxx` calls up so they precede `loadSceneFile`. Verify by re-running the sandbox at the end of this task.

- [ ] **Step 6: Update `tests/test_scene_io.cpp` for the new API**

Edit `tests/test_scene_io.cpp`:

Add includes near the existing ones:

```cpp
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
```

Add a helper near `tempScenePath`:

```cpp
iron::Reflection makeReflectionRegistry() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    return r;
}
```

In Test 1, replace the save/load call pair:

Old:
```cpp
        CHECK(saveSceneFile(original, path));

        const auto loadedOpt = loadSceneFile(path);
```

New:
```cpp
        const iron::Reflection r = makeReflectionRegistry();
        CHECK(saveSceneFile(r, original, path));

        const auto loadedOpt = loadSceneFile(r, path);
```

In Test 2 and Test 3, update the `loadSceneFile` calls:

Old (Test 2):
```cpp
        const auto loaded = loadSceneFile(path);
```

New:
```cpp
        const iron::Reflection r = makeReflectionRegistry();
        const auto loaded = loadSceneFile(r, path);
```

Old (Test 3):
```cpp
        const auto loaded = loadSceneFile("does/not/exist/scene.json");
```

New:
```cpp
        const iron::Reflection r = makeReflectionRegistry();
        const auto loaded = loadSceneFile(r, "does/not/exist/scene.json");
```

In Test 4, update the hand-authored JSON to use the nested format AND update the load call:

Old:
```cpp
        {
            std::ofstream f(path);
            f << R"({ "entities": [ { "name": "c", "mesh": { "primitive": "cube" } } ] })";
        }
        const auto loadedOpt = loadSceneFile(path);
```

New:
```cpp
        {
            std::ofstream f(path);
            f << R"({ "entities": [ { "name": "c", "mesh": { "primitive": "cube" } } ] })";
        }
        const iron::Reflection r = makeReflectionRegistry();
        const auto loadedOpt = loadSceneFile(r, path);
```

Add a new test case after Test 4 — verify the saved JSON contains nested `transform`:

```cpp
    // --- Test 5: save emits nested "transform" (no top-level position/rotation/scale) ---
    {
        SceneFile s;
        SceneEntity e;
        e.name = "x";
        e.transform.position = {1.0f, 2.0f, 3.0f};
        e.transform.scale    = {4.0f, 5.0f, 6.0f};
        e.mesh.primitive = PrimitiveKind::Cube;
        s.entities.push_back(e);

        const iron::Reflection r = makeReflectionRegistry();
        const std::string path = tempScenePath("iron_scene_nested.json");
        CHECK(saveSceneFile(r, s, path));

        std::ifstream f(path);
        std::string contents((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        f.close();
        CHECK(contents.find("\"transform\"")  != std::string::npos);
        // Make sure the entity-level flat keys do NOT appear adjacent to "name".
        // Quick sanity: the substring "],\n      \"scale\"" should not occur at
        // the top of the entity block. We do a coarse check — the word "scale"
        // should appear inside the "transform" block, not before it.
        const size_t firstName  = contents.find("\"name\"");
        const size_t firstTrans = contents.find("\"transform\"");
        CHECK(firstName  != std::string::npos);
        CHECK(firstTrans != std::string::npos);
        CHECK(firstTrans > firstName);   // transform follows name in the entity object
        fs::remove(path);
    }
```

- [ ] **Step 7: Build everything + run full suite**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build, 50/50 tests green (49 old + new test_reflection_io from C2; test_scene_io still counts as one).

- [ ] **Step 8: Smoke-launch the sandbox**

```bash
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: scene loads from migrated `demo.json`; floor + 3 cubes + helmet render; click-select shows outline; Inspector shows Transform / Mesh / Material headers; emissive renders as color picker; reflectivity as a 0..1 slider; mesh primitive shows a "cube" / "plane" dropdown (or "None" for the helmet); gltfPath is an editable text field. Close via X. (Headless agents: report N/A — the user verifies at E1.)

- [ ] **Step 9: Commit**

```bash
git add engine/editor/SceneInspector.h engine/editor/SceneInspector.cpp engine/scene/SceneIO.h engine/scene/SceneIO.cpp games/11-sandbox/main.cpp games/11-sandbox/assets/scenes/demo.json tests/test_scene_io.cpp
git commit -m "M39: wire SceneInspector + SceneIO onto ReflectionIO/ReflectionInspector; migrate demo.json"
```

---

## Phase E — Verification + PR + merge

### Task E1: Full visual gate + push + PR + squash-merge + memory update

- [ ] **Step 1: Full clean build + test suite**

```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 50/50 green (49 prior + `test_reflection_io`).

- [ ] **Step 2: User visual gate**

Hand back to the user with this checklist:

```
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

| Action | Expected |
|---|---|
| Scene loads from `demo.json` | Floor + 3 cubes + helmet render identically to M38 |
| Click any entity | Outline appears as before; Inspector shows Name + Gizmo Space + Selection Effect (hand-rolled prelude) followed by Transform / MeshRef / MaterialDef headers (reflection-driven) |
| Drag a translate handle | Position fields update live in Inspector |
| Drag a rotate handle | Rotation Euler fields update live in Inspector |
| Drag a scale handle | Scale fields update live; cannot go below 0.001 (clamp from M38) |
| Emissive widget | Renders as a color picker, not three drag fields |
| Reflectivity widget | Renders as a 0..1 slider, not a free drag |
| Mesh primitive dropdown | Shows "None / cube / plane"; switching cube→plane immediately re-meshes |
| Mesh gltfPath text field | Editable (was display-only in M38) |
| Save (the existing hotkey) | demo.json is written back in nested `"transform": {…}` format |
| Reload sandbox | Scene loads correctly from the just-written file |
| Window resize | Render + picking + gizmo unchanged (M37.5 behavior preserved) |
| Minimize / restore | No crash |

If any row regresses, return to D1 (or the relevant earlier task) and fix before proceeding.

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/m39-reflection-driven-editor
```

- [ ] **Step 4: Open the PR**

```bash
gh pr create --title "M39: reflection-driven Inspector + SceneIO; unify SceneEntity with Transform" --body "$(cat <<'EOF'
## Summary

M39 — third milestone of the foundation track. Consumes M38's
``iron::Reflection`` in two places: the editor Inspector renders fields
from ``fieldsOf<T>()`` via a TypeId dispatch, and ``SceneIO`` serializes
entity components through the same registry. Unifies ``SceneEntity``
with M37's ``Transform`` (drops the duplicated bare ``position`` /
``rotation`` / ``scale`` fields). Adds an ``EnumBuilder`` so dropdowns
and JSON enum strings are registry-driven.

- **New module** ``engine/editor/ReflectionInspector.{h,cpp}``:
  generic ``renderComponent<T>`` template + ImGui ``TypeId`` dispatch
  (Bool / Int* / Float / String / Vec3 / Quat / Enum / OptionalEnum).
  Widget hints (color, slider, dragSpeed) consulted via ``FieldMeta``.
  Quat fields rendered as euler-decomposed DragFloat3.
- **New module** ``engine/scene/ReflectionIO.{h,cpp}``:
  symmetric ``componentToJson<T>`` / ``componentFromJson<T>``. Empty
  strings omitted; nullopt enums omitted; unknown enum names log + fall
  back to the first registered value.
- **Reflection schema extension**: ``FieldMeta`` gains ``dragSpeed``,
  ``color``, ``slider``. ``FieldDesc`` gains ``enumTypeId`` (populated
  by ``TypeBuilder::field`` via a concept-constrained
  ``enumTypeIdOf<F>()`` helper, so dispatch can look up enum value
  lists without knowing the concrete ``E``). ``Reflection`` grows
  ``EnumBuilder<E>``, ``registerEnum<E>``, ``enumValues<E>`` /
  ``enumName<E>``, and non-template ``enumValuesById`` /
  ``enumNameById``.
- **``SceneEntity`` refactor**: ``Transform transform`` member
  replaces bare ``position`` / ``rotation`` / ``scale``. Mechanical
  ``e.position`` → ``e.transform.position`` rename across
  ``SceneInspector`` / ``Gizmo`` / ``SceneIO`` / sandbox / tests.
  The M37 Inspector → World sync collapses from a three-field copy to
  a single struct assignment.
- **Sidecar updates**: ``MeshRef.reflect.cpp`` registers
  ``PrimitiveKind`` enum (``"cube"`` / ``"plane"``);
  ``MaterialDef.reflect.cpp`` carries ``.color = true`` on emissive
  and ``.slider = true`` on reflectivity.
- **Editor UX bonus**: ``MeshRef::gltfPath`` is now editable (was
  display-only); primitive shows a dropdown with a "None" entry.
- **On-disk format change**: scene JSON nests ``transform`` /
  ``mesh`` / ``material`` per entity. No backward-compat fallback —
  ``games/11-sandbox/assets/scenes/demo.json`` is hand-migrated as
  part of M39.
- **Tests**: new ``tests/test_reflection_io.cpp`` (~8 named subtests
  for roundtrips, enum handling, hint-vs-serialization, no-load-clamp).
  ``tests/test_scene_io.cpp`` ported to the new nested format with a
  new "save emits nested transform" assertion. 49 → 50 tests.

## Test plan

- [x] Full suite green (50/50)
- [x] ironcore + sandbox build clean
- [x] Visual: Inspector renders Transform / MeshRef / MaterialDef from
      reflection; mesh primitive dropdown; gltfPath editable; emissive
      ColorEdit3; reflectivity slider
- [x] Save → reload roundtrip on the migrated demo.json

## Known v1 limitations (intentional, deferred)

- World migration (editor reads ``iron::World`` directly;
  ``resolved[]`` deleted; sync hack removed) — separate milestone.
- Nested-struct reflection (SceneEntity itself reflectable) — defer.
- ``sun`` / ``fog`` / ``pointLights`` / ``clearColor`` through
  reflection — possible follow-up.
- ``readonly`` / ``hidden`` / ``tooltip`` / ``category`` field flags
  — YAGNI.
- Enum underlying-type fixed at int32 in the dispatch — future enums
  declared with ``: uint8_t`` etc. would need a templated read/write
  pair. Flag in the codebase if we add such an enum.
- Macro sugar (``REFLECT_BEGIN``) — sidecar pattern is v1.
- Reflection of script-defined types + AngelScript binding — M40.
- Class-with-methods OO component layer — M40.
EOF
)"
```

- [ ] **Step 5: Watch CI**

```bash
gh pr checks --watch
```

If CI fails on a transient issue (vcpkg / Kitware mirror flake), re-run via `gh run rerun <run-id> --failed`.

- [ ] **Step 6: Squash-merge**

When CI is green:

```bash
gh pr merge --squash --delete-branch
git checkout main && git pull --ff-only origin main
git log --oneline -3
```

- [ ] **Step 7: Update memory**

After merge, update three files:

- In `MEMORY.md` index, bump the `iron-core-engine-progress` line to mention M39 merged (PR # and new SHA from `git log`). Note that the foundation track now has three landed milestones (M37, M38, M39) and the next options are: (a) World migration, (b) render-polish detour, (c) editor-authoring (collision-shape, audio-emitter).
- In `iron-core-engine-progress.md`, append an `M39 — reflection-driven Inspector + SceneIO` entry near the M38 entry with the merge SHA, a one-paragraph summary, and the v1 limits list from the PR body. Note the SceneEntity-contains-Transform unification.
- In `iron-core-engine-roadmap.md`, mark M39 done and update the "next options after M38" line to remove M39 from the foundation-track choice and surface the three remaining post-M39 branches (World migration / render polish / editor authoring).

---

## Acceptance criteria

1. Inspector renders Transform + MeshRef + MaterialDef via reflection — no hand-rolled per-field widgets remain in the component body (editor-tool prelude excepted).
2. `MeshRef::primitive` dropdown shows "None / cube / plane"; selecting "None" clears the optional.
3. `MeshRef::gltfPath` is now editable as a text field.
4. `MaterialDef::emissive` renders as `ColorEdit3`.
5. `MaterialDef::reflectivity` renders as `SliderFloat` 0..1.
6. `SceneIO::saveSceneFile` / `loadSceneFile` route through `componentToJson` / `componentFromJson`.
7. `demo.json` migrated in-place to the nested format; loads cleanly under the new SceneIO.
8. Saves emit nested `"transform": {…}` and do NOT include top-level position/rotation/scale keys; reading legacy flat-format JSON produces a default transform (no silent migration).
9. `SceneEntity` no longer has bare position/rotation/scale fields.
10. All editor flows still work: select, gizmo drag (translate / rotate / scale), add/delete/duplicate, save/load.
11. 50 / 50 tests green.
12. Sandbox visually indistinguishable from M38 (M39 is a behavior-preserving refactor + minor editing-affordance expansion: mesh fields now editable).
13. Renderer / picking / shipping games untouched.
