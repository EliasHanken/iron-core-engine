# M38 — Reflection / type registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `iron::Reflection` (host-owned type registry) + register the four M37 POD components via sidecar `register<TypeName>(Reflection&)` free functions. Pure-logic milestone — no editor or serialization changes. M39 picks up the registry to drive Inspector + JSON.

**Architecture:** Per-type metadata is captured as an ordered `std::vector<FieldDesc>` keyed by `componentTypeId<T>()` (reused from M37). The fluent `TypeBuilder<T>::field(name, &T::member)` deduces the field's `TypeId` via `TypeIdOf<F>::v` specializations, computes the byte offset via a transient `T{}` probe (standard-layout types only — `static_assert`-guarded). Sidecar `.cpp` files next to each type expose a single `void register<TypeName>(Reflection&)` function that the sandbox host calls at startup. No singletons; no macros.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`). No new external dependencies. Reference spec: `docs/superpowers/specs/2026-05-31-m38-reflection-design.md`.

**Verification model:** Whole reflection API is **pure logic** — no Vulkan, no editor, no sandbox runtime needed for any unit test. Every task's mechanical gate: **builds clean** (`cmake --build build-vk --config Debug --target ironcore`) and the **existing 48 tests stay green** (`ctest --test-dir build-vk -C Debug`). Phase D adds visual gate (sandbox still works — M38 is runtime-invisible).

**Build & test commands (used by every task):**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_type_reflection   # after Task A1 lands
cmake --build build-vk --config Debug --target sandbox            # for Phase C/D
ctest --test-dir build-vk -C Debug --output-on-failure
```
(A benign "LF will be replaced by CRLF" git warning is expected on Windows. Pre-existing ImGui/GLFW `LNK4217` linker warnings are benign.)

**Branch:** already on `feat/m38-reflection` (the spec commit `d034c4c` sits on the branch tip). Every task in this plan commits to this branch.

---

## File Structure

**New (pure-logic, header-only or near-header-only):**
- `engine/reflection/TypeId.h` — `TypeId` enum (Unknown, Bool, Int32, UInt32, UInt8, Float, String, Vec3, Quat, Enum, OptionalEnum)
- `engine/reflection/FieldDesc.h` — `FieldMeta` + `FieldDesc` (with templated `ptr<T>(void*)` accessor)
- `engine/reflection/TypeIdOf.h` — `TypeIdOf<F>::v` template specializations
- `engine/reflection/Reflection.h` — `Reflection` class + nested `TypeBuilder<T>`; fluent `registerType<T>().field(name, &T::member)` + lookup API (`fieldsOf<T>`, `fieldByName<T>`, `typeName<T>`)
- `engine/reflection/RegisterCoreTypes.h` — forward declarations for the four sidecar `register*` functions
- `engine/world/Transform.reflect.cpp` — `void registerTransform(Reflection&)`
- `engine/scene/MeshRef.reflect.cpp` — `void registerMeshRef(Reflection&)`
- `engine/scene/MaterialDef.reflect.cpp` — `void registerMaterialDef(Reflection&)`
- `engine/render/RenderHandles.reflect.cpp` — `void registerRenderHandles(Reflection&)`
- `tests/test_type_reflection.cpp` — hand-rolled CHECK macro + ~15–20 named subtests

**Modified:**
- `engine/CMakeLists.txt` — add the four `.reflect.cpp` files to the `ironcore` source list
- `tests/CMakeLists.txt` — `iron_add_test(test_type_reflection test_type_reflection.cpp)`
- `games/11-sandbox/main.cpp` — include the headers, construct `iron::Reflection reflection;`, call the four `register*(reflection)` functions at startup

**Untouched on purpose:** Renderer (Vulkan), World/ComponentArray, Picking, Gizmo, editor panels, SceneIO, all other games. No new `.cpp` for `Reflection` itself — everything is header-only (templated) in v1.

**Phases:**
- **A** — Reflection scaffold (TypeId, FieldDesc, TypeIdOf, Reflection class). Pure logic, TDD. 4 tasks.
- **B** — Sidecar registrations for the 4 M37 components + integration tests. 1 task.
- **C** — Sandbox host wiring (RegisterCoreTypes.h + ~5 lines in main.cpp). 1 task.
- **D** — Visual gate + push + PR + merge + memory. 1 task.

---

## Phase A — Reflection scaffold (pure logic, TDD)

### Task A1: `TypeId` enum + `FieldDesc` + `FieldMeta` + test scaffold

**Files:**
- Create: `engine/reflection/TypeId.h`
- Create: `engine/reflection/FieldDesc.h`
- Create: `tests/test_type_reflection.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_type_reflection.cpp`:

```cpp
#include "reflection/TypeId.h"
#include "reflection/FieldDesc.h"

#include <cstdint>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_typeid_unknown_is_zero() {
    CHECK(static_cast<int>(iron::TypeId::Unknown) == 0);
}

static void test_fieldmeta_defaults_are_zero() {
    iron::FieldMeta m;
    CHECK(m.min == 0.0f);
    CHECK(m.max == 0.0f);
}

static void test_fielddesc_ptr_arithmetic() {
    // Simulate a struct with two fields and verify ptr<T> computes the right
    // address via the stored byte offset.
    struct Probe { int a; float b; };
    Probe p{};
    iron::FieldDesc fa{ "a", iron::TypeId::Int32,
                        static_cast<uint32_t>(offsetof(Probe, a)), {} };
    iron::FieldDesc fb{ "b", iron::TypeId::Float,
                        static_cast<uint32_t>(offsetof(Probe, b)), {} };
    CHECK(fa.ptr<int>(&p) == &p.a);
    CHECK(fb.ptr<float>(&p) == &p.b);
    *fa.ptr<int>(&p)   = 42;
    *fb.ptr<float>(&p) = 1.5f;
    CHECK(p.a == 42);
    CHECK(p.b == 1.5f);
}

int main() {
    test_typeid_unknown_is_zero();
    test_fieldmeta_defaults_are_zero();
    test_fielddesc_ptr_arithmetic();
    if (g_failures == 0) std::printf("All reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
```

Add the test registration to `tests/CMakeLists.txt` (append after the last `iron_add_test` line):
```cmake
iron_add_test(test_type_reflection test_type_reflection.cpp)
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `reflection/TypeId.h` does not exist.

- [ ] **Step 3: Implement `TypeId.h`**

Create `engine/reflection/TypeId.h`:

```cpp
#pragma once

#include <cstdint>

namespace iron {

// Reflection type tags. v1 covers what the four M37 POD components need;
// extend the enum as new field types appear.
enum class TypeId : uint8_t {
    Unknown,
    Bool,
    Int32,
    UInt32,
    UInt8,
    Float,
    String,
    Vec3,
    Quat,
    Enum,
    OptionalEnum,
};

}  // namespace iron
```

- [ ] **Step 4: Implement `FieldDesc.h`**

Create `engine/reflection/FieldDesc.h`:

```cpp
#pragma once

#include "reflection/TypeId.h"

#include <cstdint>
#include <string_view>

namespace iron {

// Per-field metadata. v1 covers range clamps (used by the Inspector in M39).
struct FieldMeta {
    float min = 0.0f;   // both zero = no clamp
    float max = 0.0f;
};

// One reflected field. Name has static storage duration (string literal
// from the sidecar .cpp); the registry stores it as a string_view.
struct FieldDesc {
    std::string_view name;
    TypeId           type   = TypeId::Unknown;
    uint32_t         offset = 0;
    FieldMeta        meta   = {};

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

}  // namespace iron
```

- [ ] **Step 5: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: `All reflection tests passed.` + `Passed`.

- [ ] **Step 6: Commit**

From repo root:
```bash
git add engine/reflection/TypeId.h engine/reflection/FieldDesc.h tests/test_type_reflection.cpp tests/CMakeLists.txt
git commit -m "M38: TypeId + FieldDesc + test scaffold"
```

Run `git log --oneline -3` and include the SHA in your report.

---

### Task A2: `TypeIdOf<F>::v` template specializations

**Files:**
- Create: `engine/reflection/TypeIdOf.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
#include "reflection/TypeIdOf.h"
#include "math/Vec.h"
#include "math/Quaternion.h"
#include "scene/SceneFormat.h"   // for PrimitiveKind enum in deduction tests

#include <optional>
#include <string>

static void test_typeidof_primitives() {
    CHECK(iron::TypeIdOf<bool>::v     == iron::TypeId::Bool);
    CHECK(iron::TypeIdOf<int32_t>::v  == iron::TypeId::Int32);
    CHECK(iron::TypeIdOf<uint32_t>::v == iron::TypeId::UInt32);
    CHECK(iron::TypeIdOf<uint8_t>::v  == iron::TypeId::UInt8);
    CHECK(iron::TypeIdOf<float>::v    == iron::TypeId::Float);
}

static void test_typeidof_string() {
    CHECK(iron::TypeIdOf<std::string>::v == iron::TypeId::String);
}

static void test_typeidof_vec3_quat() {
    CHECK(iron::TypeIdOf<iron::Vec3>::v == iron::TypeId::Vec3);
    CHECK(iron::TypeIdOf<iron::Quat>::v == iron::TypeId::Quat);
}

static void test_typeidof_enum() {
    CHECK(iron::TypeIdOf<iron::PrimitiveKind>::v == iron::TypeId::Enum);
}

static void test_typeidof_optional_enum() {
    CHECK(iron::TypeIdOf<std::optional<iron::PrimitiveKind>>::v
          == iron::TypeId::OptionalEnum);
}
```

Register the new tests in `main()` after the existing A1 calls, before the success print:
```cpp
    test_typeidof_primitives();
    test_typeidof_string();
    test_typeidof_vec3_quat();
    test_typeidof_enum();
    test_typeidof_optional_enum();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `reflection/TypeIdOf.h` does not exist.

- [ ] **Step 3: Implement `TypeIdOf.h`**

Create `engine/reflection/TypeIdOf.h`:

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/TypeId.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace iron {

// Member-pointer F -> TypeId mapping. v1 covers everything the four M37
// POD components need. Extend with new primary-template specializations
// when new field types appear; enum/optional are concept-constrained.

template <class F> struct TypeIdOf;

template <> struct TypeIdOf<bool>          { static constexpr TypeId v = TypeId::Bool; };
template <> struct TypeIdOf<int32_t>       { static constexpr TypeId v = TypeId::Int32; };
template <> struct TypeIdOf<uint32_t>      { static constexpr TypeId v = TypeId::UInt32; };
template <> struct TypeIdOf<uint8_t>       { static constexpr TypeId v = TypeId::UInt8; };
template <> struct TypeIdOf<float>         { static constexpr TypeId v = TypeId::Float; };
template <> struct TypeIdOf<std::string>   { static constexpr TypeId v = TypeId::String; };
template <> struct TypeIdOf<Vec3>          { static constexpr TypeId v = TypeId::Vec3; };
template <> struct TypeIdOf<Quat>          { static constexpr TypeId v = TypeId::Quat; };

template <class E> requires std::is_enum_v<E>
struct TypeIdOf<E> { static constexpr TypeId v = TypeId::Enum; };

template <class E> requires std::is_enum_v<E>
struct TypeIdOf<std::optional<E>> { static constexpr TypeId v = TypeId::OptionalEnum; };

}  // namespace iron
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all reflection tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/TypeIdOf.h tests/test_type_reflection.cpp
git commit -m "M38: TypeIdOf<F>::v deduction + tests"
```

---

### Task A3: `Reflection::registerType` + `TypeBuilder<T>::field`

**Files:**
- Create: `engine/reflection/Reflection.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
#include "reflection/Reflection.h"
#include "world/Transform.h"

static void test_type_reflection_register_type_stores_name() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform");
    CHECK(r.typeName<iron::Transform>() == "Transform");
}

static void test_type_reflection_unregistered_type_has_empty_name() {
    iron::Reflection r;
    CHECK(r.typeName<iron::Transform>().empty());
}

static void test_type_reflection_field_offsets_match_offsetof() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position)
        .field("rotation", &iron::Transform::rotation)
        .field("scale",    &iron::Transform::scale);
    auto fields = r.fieldsOf<iron::Transform>();
    CHECK(fields.size() == 3);
    CHECK(fields[0].name == "position");
    CHECK(fields[1].name == "rotation");
    CHECK(fields[2].name == "scale");
    CHECK(fields[0].offset == offsetof(iron::Transform, position));
    CHECK(fields[1].offset == offsetof(iron::Transform, rotation));
    CHECK(fields[2].offset == offsetof(iron::Transform, scale));
    CHECK(fields[0].type == iron::TypeId::Vec3);
    CHECK(fields[1].type == iron::TypeId::Quat);
    CHECK(fields[2].type == iron::TypeId::Vec3);
}

static void test_type_reflection_field_meta_roundtrip() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("scale", &iron::Transform::scale, {.min = 0.001f, .max = 1000.0f});
    auto fields = r.fieldsOf<iron::Transform>();
    CHECK(fields.size() == 1);
    CHECK(fields[0].meta.min == 0.001f);
    CHECK(fields[0].meta.max == 1000.0f);
}
```

Register in `main()`:
```cpp
    test_type_reflection_register_type_stores_name();
    test_type_reflection_unregistered_type_has_empty_name();
    test_type_reflection_field_offsets_match_offsetof();
    test_type_reflection_field_meta_roundtrip();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `reflection/Reflection.h` does not exist.

- [ ] **Step 3: Implement `Reflection.h`**

Create `engine/reflection/Reflection.h`:

```cpp
#pragma once

#include "reflection/FieldDesc.h"
#include "reflection/TypeIdOf.h"
#include "world/Entity.h"   // componentTypeId<T>()

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iron {

// Host-owned type registry. Reuses M37's componentTypeId<T>() for the
// per-type integer key (256-type cap is generous at this scale).
class Reflection {
public:
    static constexpr uint32_t kMaxTypes = 256;

    template <class T>
    class TypeBuilder {
    public:
        template <class F>
        TypeBuilder& field(std::string_view name, F T::* member, FieldMeta meta = {}) {
            // Offset computation: a transient default-constructed T plus
            // pointer arithmetic. Well-defined for standard-layout types
            // (the static_assert in registerType<T> guards this).
            T probe{};
            const uint32_t off = static_cast<uint32_t>(
                reinterpret_cast<const uint8_t*>(&(probe.*member)) -
                reinterpret_cast<const uint8_t*>(&probe));
            reg_.types_[typeId_].fields.push_back(
                FieldDesc{ name, TypeIdOf<F>::v, off, meta });
            return *this;
        }

    private:
        friend class Reflection;
        TypeBuilder(Reflection& r, uint32_t typeId) : reg_(r), typeId_(typeId) {}
        Reflection& reg_;
        uint32_t    typeId_;
    };

    template <class T>
    TypeBuilder<T> registerType(std::string_view name) {
        static_assert(std::is_standard_layout_v<T>,
            "Reflection requires standard-layout types (offset computation depends on it).");
        static_assert(std::is_default_constructible_v<T>,
            "Reflection requires default-constructible types (the offset probe is T{}).");
        const uint32_t id = componentTypeId<T>();
        assert(id < kMaxTypes && "Too many reflected types (raise Reflection::kMaxTypes)");
        types_[id].name = name;
        types_[id].fields.clear();
        registered_[id] = true;
        return TypeBuilder<T>(*this, id);
    }

    template <class T>
    std::span<const FieldDesc> fieldsOf() const {
        const uint32_t id = componentTypeId<T>();
        return registered_[id] ? std::span<const FieldDesc>(types_[id].fields)
                               : std::span<const FieldDesc>{};
    }

    template <class T>
    std::string_view typeName() const {
        const uint32_t id = componentTypeId<T>();
        return registered_[id] ? types_[id].name : std::string_view{};
    }

private:
    struct TypeEntry {
        std::string_view       name;
        std::vector<FieldDesc> fields;
    };
    std::array<TypeEntry, kMaxTypes> types_{};
    std::array<bool, kMaxTypes>      registered_{};
};

}  // namespace iron
```

(Note: `fieldByName<T>` lookup lands in A4 — A3 only ships `registerType`, `field`, `fieldsOf`, `typeName`.)

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/Reflection.h tests/test_type_reflection.cpp
git commit -m "M38: Reflection::registerType + TypeBuilder<T>::field"
```

---

### Task A4: `Reflection::fieldByName` + integration test through a real type

**Files:**
- Modify: `engine/reflection/Reflection.h`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
static void test_type_reflection_field_by_name_hit() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position)
        .field("rotation", &iron::Transform::rotation)
        .field("scale",    &iron::Transform::scale);
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    CHECK(f->name == "position");
    CHECK(f->type == iron::TypeId::Vec3);
}

static void test_type_reflection_field_by_name_miss() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    CHECK(r.fieldByName<iron::Transform>("nonexistent") == nullptr);
}

static void test_type_reflection_ptr_through_field_mutates_object() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    iron::Transform t{};
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    iron::Vec3* p = f->ptr<iron::Vec3>(&t);
    p->x = 1.0f;
    p->y = 2.0f;
    p->z = 3.0f;
    CHECK(t.position.x == 1.0f);
    CHECK(t.position.y == 2.0f);
    CHECK(t.position.z == 3.0f);
}

static void test_type_reflection_const_ptr_through_field() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    iron::Transform t{};
    t.position = iron::Vec3{4.0f, 5.0f, 6.0f};
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    const iron::Vec3* p = f->ptr<iron::Vec3>(static_cast<const void*>(&t));
    CHECK(p->x == 4.0f);
    CHECK(p->y == 5.0f);
    CHECK(p->z == 6.0f);
}
```

Register in `main()`:
```cpp
    test_type_reflection_field_by_name_hit();
    test_type_reflection_field_by_name_miss();
    test_type_reflection_ptr_through_field_mutates_object();
    test_type_reflection_const_ptr_through_field();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `r.fieldByName<...>` not declared.

- [ ] **Step 3: Implement `fieldByName`**

Add to `engine/reflection/Reflection.h` inside `class Reflection`'s public section, after `typeName`:

```cpp
    template <class T>
    const FieldDesc* fieldByName(std::string_view name) const {
        for (const FieldDesc& f : fieldsOf<T>())
            if (f.name == name) return &f;
        return nullptr;
    }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug -R test_type_reflection --output-on-failure -V
```
Expected: all reflection tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/reflection/Reflection.h tests/test_type_reflection.cpp
git commit -m "M38: Reflection::fieldByName + ptr-through-field tests"
```

---

## Phase B — Sidecar registrations for the 4 M37 components

### Task B1: Sidecar `.reflect.cpp` for all four component types + integration tests

**Files:**
- Create: `engine/world/Transform.reflect.cpp`
- Create: `engine/scene/MeshRef.reflect.cpp`
- Create: `engine/scene/MaterialDef.reflect.cpp`
- Create: `engine/render/RenderHandles.reflect.cpp`
- Create: `engine/reflection/RegisterCoreTypes.h`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/test_type_reflection.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_type_reflection.cpp` BEFORE `int main()`:

```cpp
#include "reflection/RegisterCoreTypes.h"
#include "render/RenderHandles.h"

static void test_register_transform_end_to_end() {
    iron::Reflection r;
    iron::registerTransform(r);
    CHECK(r.typeName<iron::Transform>() == "Transform");
    auto f = r.fieldsOf<iron::Transform>();
    CHECK(f.size() == 3);
    CHECK(f[0].name == "position");
    CHECK(f[1].name == "rotation");
    CHECK(f[2].name == "scale");
    CHECK(f[2].meta.min == 0.001f);   // scale lower bound
}

static void test_register_mesh_ref_end_to_end() {
    iron::Reflection r;
    iron::registerMeshRef(r);
    CHECK(r.typeName<iron::MeshRef>() == "MeshRef");
    auto f = r.fieldsOf<iron::MeshRef>();
    CHECK(f.size() == 2);
    CHECK(f[0].name == "primitive");
    CHECK(f[0].type == iron::TypeId::OptionalEnum);
    CHECK(f[1].name == "gltfPath");
    CHECK(f[1].type == iron::TypeId::String);
}

static void test_register_material_def_end_to_end() {
    iron::Reflection r;
    iron::registerMaterialDef(r);
    CHECK(r.typeName<iron::MaterialDef>() == "MaterialDef");
    auto f = r.fieldsOf<iron::MaterialDef>();
    CHECK(f.size() == 6);
    CHECK(f[0].name == "albedoPath");
    CHECK(f[3].name == "emissive");
    CHECK(f[3].type == iron::TypeId::Vec3);
    const iron::FieldDesc* refl = r.fieldByName<iron::MaterialDef>("reflectivity");
    CHECK(refl != nullptr);
    CHECK(refl->meta.min == 0.0f);
    CHECK(refl->meta.max == 1.0f);
}

static void test_register_render_handles_end_to_end() {
    iron::Reflection r;
    iron::registerRenderHandles(r);
    CHECK(r.typeName<iron::RenderHandles>() == "RenderHandles");
    auto f = r.fieldsOf<iron::RenderHandles>();
    CHECK(f.size() == 4);
    CHECK(f[0].name == "mesh");
    CHECK(f[0].type == iron::TypeId::UInt32);   // MeshHandle is uint32_t
    CHECK(f[1].name == "albedo");
    CHECK(f[1].type == iron::TypeId::UInt32);
}

static void test_register_all_four_in_one_registry() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    CHECK(!r.typeName<iron::Transform>().empty());
    CHECK(!r.typeName<iron::MeshRef>().empty());
    CHECK(!r.typeName<iron::MaterialDef>().empty());
    CHECK(!r.typeName<iron::RenderHandles>().empty());
}
```

Register in `main()`:
```cpp
    test_register_transform_end_to_end();
    test_register_mesh_ref_end_to_end();
    test_register_material_def_end_to_end();
    test_register_render_handles_end_to_end();
    test_register_all_four_in_one_registry();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_type_reflection
```
Expected: compile error — `reflection/RegisterCoreTypes.h` does not exist.

- [ ] **Step 3: Implement `RegisterCoreTypes.h`**

Create `engine/reflection/RegisterCoreTypes.h`:

```cpp
#pragma once

namespace iron {

class Reflection;

void registerTransform(Reflection& r);
void registerMeshRef(Reflection& r);
void registerMaterialDef(Reflection& r);
void registerRenderHandles(Reflection& r);

}  // namespace iron
```

- [ ] **Step 4: Implement the four sidecar `.reflect.cpp` files**

Create `engine/world/Transform.reflect.cpp`:

```cpp
#include "reflection/Reflection.h"
#include "world/Transform.h"

namespace iron {

void registerTransform(Reflection& r) {
    r.registerType<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation)
        .field("scale",    &Transform::scale, {.min = 0.001f});
}

}  // namespace iron
```

Create `engine/scene/MeshRef.reflect.cpp`:

```cpp
#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMeshRef(Reflection& r) {
    r.registerType<MeshRef>("MeshRef")
        .field("primitive", &MeshRef::primitive)
        .field("gltfPath",  &MeshRef::gltfPath);
}

}  // namespace iron
```

Create `engine/scene/MaterialDef.reflect.cpp`:

```cpp
#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMaterialDef(Reflection& r) {
    r.registerType<MaterialDef>("MaterialDef")
        .field("albedoPath",   &MaterialDef::albedoPath)
        .field("normalPath",   &MaterialDef::normalPath)
        .field("specularPath", &MaterialDef::specularPath)
        .field("emissive",     &MaterialDef::emissive)
        .field("uvScale",      &MaterialDef::uvScale,      {.min = 0.0f, .max = 100.0f})
        .field("reflectivity", &MaterialDef::reflectivity, {.min = 0.0f, .max = 1.0f});
}

}  // namespace iron
```

Create `engine/render/RenderHandles.reflect.cpp`:

```cpp
#include "reflection/Reflection.h"
#include "render/RenderHandles.h"

namespace iron {

void registerRenderHandles(Reflection& r) {
    r.registerType<RenderHandles>("RenderHandles")
        .field("mesh",     &RenderHandles::mesh)
        .field("albedo",   &RenderHandles::albedo)
        .field("normal",   &RenderHandles::normal)
        .field("specular", &RenderHandles::specular);
}

}  // namespace iron
```

- [ ] **Step 5: Register the four sidecars in `engine/CMakeLists.txt`**

Open `engine/CMakeLists.txt`. Find the `add_library(ironcore STATIC ...)` source list (starts at line 1 with `core/Log.cpp`). Add these four lines, anywhere sensible in the list (the existing list has no strict ordering — append to the end works):

```cmake
  render/RenderHandles.reflect.cpp
  scene/MaterialDef.reflect.cpp
  scene/MeshRef.reflect.cpp
  world/Transform.reflect.cpp
```

- [ ] **Step 6: Build + run tests**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_type_reflection
cd build-vk && ctest -C Debug --output-on-failure
```
Expected: clean build; 49/49 tests green (48 prior + new `test_type_reflection`).

- [ ] **Step 7: Commit**

```bash
git add engine/reflection/RegisterCoreTypes.h engine/world/Transform.reflect.cpp engine/scene/MeshRef.reflect.cpp engine/scene/MaterialDef.reflect.cpp engine/render/RenderHandles.reflect.cpp engine/CMakeLists.txt tests/test_type_reflection.cpp
git commit -m "M38: sidecar registrations for the 4 M37 component types + integration tests"
```

---

## Phase C — Sandbox host wiring

### Task C1: Construct `Reflection` in the sandbox and call the four `register*` functions

**Files:**
- Modify: `games/11-sandbox/main.cpp`

The sandbox does not USE the reflection registry in M38 — it just owns and populates it. M39 picks it up. This task adds ~6 lines.

- [ ] **Step 1: Locate the sandbox World construction**

Open `games/11-sandbox/main.cpp` and grep for `iron::World world;` — the host's `World` member, declared as a local variable near the other host-owned state (around line 308 or so post-M37). The reflection registry goes immediately after it, near the existing setup.

- [ ] **Step 2: Add the include + construction + registration calls**

Add these includes near the existing engine includes (probably alphabetical near `render/RenderHandles.h` / `world/World.h`):

```cpp
#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
```

Add the registry construction + registrations near the World declaration. Place these lines immediately after `iron::World world;`:

```cpp
    // M38: type registry — populated at startup; consumed by M39+ editor /
    // serialization. Sandbox doesn't use it directly in v1.
    iron::Reflection reflection;
    iron::registerTransform(reflection);
    iron::registerMeshRef(reflection);
    iron::registerMaterialDef(reflection);
    iron::registerRenderHandles(reflection);
```

- [ ] **Step 3: Build sandbox + run tests**

```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build (benign LNK4217 ImGui warnings only); 49/49 tests green.

- [ ] **Step 4: Smoke-launch the sandbox**

```bash
./build-vk/games/11-sandbox/Debug/sandbox.exe
```
Expected: identical behavior to M37.5 — scene renders, picking works, gizmo works, resize works. M38 is runtime-invisible. Close via X. (Headless agents: report N/A, the user verifies at D1.)

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M38: sandbox — construct Reflection registry + register the 4 component types at startup"
```

---

## Phase D — Verification + PR + merge

### Task D1: Full visual gate + push + PR + squash-merge + memory update

- [ ] **Step 1: Full test suite + clean build**

```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 49/49 green (48 prior + `test_type_reflection`).

- [ ] **Step 2: User visual gate**

Hand back to the user with this checklist:

```
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

| Action | Expected |
|---|---|
| Scene loads | Renders identically to M37.5 — M38 is runtime-invisible |
| Click an entity | Outline appears as before |
| Drag a gizmo handle | Behaves as before |
| Resize the window | Render + picking still correct (M37.5 fix unchanged) |
| Minimize / restore | No crash |

If any row regresses, return to C1 (sandbox wiring) or earlier; fix before proceeding.

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/m38-reflection
```

- [ ] **Step 4: Open the PR**

```bash
gh pr create --title "M38: reflection / type registry — host-owned, sidecar-registered" --body "$(cat <<'EOF'
## Summary

M38 — second milestone of the foundation track. Introduces
``iron::Reflection`` (host-owned type registry) and registers the four
M37 POD component types via sidecar ``register*(Reflection&)`` free
functions. Pure-logic milestone; runtime-invisible until M39 picks up
the registry to drive the Inspector and JSON serialization.

- **New module** ``engine/reflection/``: ``TypeId`` enum (Bool / Int32 /
  UInt32 / UInt8 / Float / String / Vec3 / Quat / Enum / OptionalEnum +
  Unknown), ``FieldDesc`` + ``FieldMeta`` (name / type / offset / min / max),
  ``TypeIdOf<F>::v`` template specializations + concept-constrained
  enum/optional cases, ``Reflection`` class (fluent ``registerType<T>().
  field(name, &T::member)`` + ``fieldsOf<T>`` / ``fieldByName<T>`` /
  ``typeName<T>``).
- **Sidecar registrations** for ``Transform`` / ``MeshRef`` / ``MaterialDef``
  / ``RenderHandles``. Each is a ``register<TypeName>(Reflection&)``
  free function in a ``.reflect.cpp`` next to the type. The host
  (``games/11-sandbox/main.cpp``) constructs ``iron::Reflection
  reflection;`` at startup and calls each ``register*`` function.
- **Tests**: new ``tests/test_type_reflection.cpp`` (single CTest case,
  ~17 named subtests). Covers ``TypeIdOf`` deduction, registry
  registration, field iteration in insertion order, ``fieldByName``
  hits + misses, ``FieldDesc::ptr<T>`` mutation roundtrip, per-field
  metadata roundtrip, and end-to-end registration of each of the four
  component types. 48 → 49 tests.

## Test plan

- [x] Full suite green (49/49)
- [x] ironcore + sandbox build clean
- [x] Visual: sandbox identical to M37.5 — feature is runtime-invisible

## Known v1 limitations (intentional, deferred)

- Editor Inspector data-driven rendering — M39.
- SceneIO reflection-driven JSON serialization — M39.
- Enum name lists (``TypeId::Enum`` tag carries no value-list) — M39
  when Inspector dropdowns need them.
- Nested-struct recursion (Vec3 stays a primitive) — defer.
- Macro sugar layer (``REFLECT_BEGIN`` / ``REFLECT_FIELD``) — sidecar
  is v1; wrap later if boilerplate bites.
- Per-field default-value capture — consumers default-construct ``T{}``
  to compare.
- Reflection of script-defined types + AngelScript binding — M40.
- Class-with-methods OO component layer (the M37 hybrid's deferred
  half) — M40.
EOF
)"
```

- [ ] **Step 5: Watch CI**

```bash
gh pr checks --watch
```

If CI fails on a transient issue (vcpkg / Kitware mirror 500 happened on PR #57), re-run via `gh run rerun <run-id> --failed`.

- [ ] **Step 6: Squash-merge**

When CI is green:

```bash
gh pr merge --squash --delete-branch
git checkout main && git pull --ff-only origin main
git log --oneline -3
```

- [ ] **Step 7: Update memory**

After merge:
- In `MEMORY.md` index entry for `iron-core-engine-progress`: update SHA + latest milestone line to note M38 merged. Mention M39 (editor + serialization migration onto reflection) as the next foundation-track step.
- In `iron-core-engine-progress.md`: append an `M38 — reflection / type registry` entry near the M37.5 entry, with the merge SHA, one-paragraph summary, and the v1 limits list from the PR body.
- In `iron-core-engine-roadmap.md`: mark M38 done; update the "next options" line near M37.5 to remove M38 from the choices and show M39 (editor migration) as the foundation-track next step.

---

## Acceptance criteria

1. `iron::Reflection` exists; registers types via the fluent `TypeBuilder<T>::field` API; supports `fieldsOf<T>` / `fieldByName<T>` / `typeName<T>` lookup; `FieldDesc::ptr<T>(void*)` returns the correct address via stored offset.
2. All four M37 component types are registered via sidecar `register<TypeName>(Reflection&)` functions; the sandbox host calls each at startup.
3. `test_type_reflection.cpp` passes ~17 subtests covering `TypeIdOf` deduction, registry operations, field iteration, by-name lookup, `FieldDesc::ptr` access, metadata roundtrip, and end-to-end registration of all four component types.
4. Suite 48 → 49 tests green; sandbox visually indistinguishable from M37.5 (M38 is runtime-invisible).
5. Zero new external dependencies. Renderer, editor panels, picking, gizmo, SceneIO, World — all untouched.
6. PR merged; memory updated.
