# M38 — Reflection / type registry (Design)

**Date:** 2026-05-31
**Milestone:** M38 (foundation track — second of Track A: M37 components → **M38 reflection** → M39 editor integration → M40 scripting → M41+ port systems)
**Prerequisite:** M37 component model (`iron::World`, `iron::componentTypeId<T>()`, the four POD components `Transform` / `MeshRef` / `MaterialDef` / `RenderHandles`).

## Goal

Make iron-core-engine's POD types **introspectable** at runtime — capture each
type's ordered list of fields (name, type tag, byte offset, optional metadata
like ranges) in a host-owned `iron::Reflection` registry. After M38 the engine
can iterate any registered type's fields generically, look up a field by name,
and read/write field values through a type-erased pointer. No consumer
migration in this milestone — M39 picks up the registry to drive the Inspector
and JSON serialization; M40 binds it to AngelScript.

This is the **structural unlock** that turns the M37 component model into
something the editor and scripting layers can talk to without per-type
hand-written code. After M38, adding a new component type means writing the
struct + one sidecar `register*(Reflection&)` function; the Inspector and
serialization keep working without per-type changes (once M39 lands them).

## Context: how other engines do this (why this shape)

- **Unreal**: `UPROPERTY` macros on fields + `UCLASS`/`USTRUCT` on types,
  parsed by UnrealHeaderTool (UHT) at build time into generated code that
  registers types with `UStruct`'s global registry. Macro + codegen.
- **Unity**: `[SerializeField]` and `MonoBehaviour` reflection driven by
  C#'s built-in `System.Reflection`. Free because C# is reflective.
- **Bevy (Rust)**: `#[derive(Reflect)]` proc macro generates trait impls;
  registration is explicit `app.register_type::<T>()` at startup. Sidecar +
  derive macro.
- **EnTT / cereal / nlohmann::json**: manual sidecar — explicit
  `struct_field(name, &T::member)` calls in a `.cpp`. No macros.

C++23 doesn't ship standard reflection; the closest thing (P2996, C++26) isn't
available in MSVC. Until then, every C++ engine uses some flavor of
macro + sidecar registration. We pick **explicit sidecar** for M38 — modern
C++23 code, debugger-friendly, IDE-friendly, and easy to wrap in macro sugar
later if the boilerplate bites. See [[feedback-prefer-unreal-patterns]] — this
is the "deliberately better than Unreal's macros" alternative: Unreal's macro
system pre-dates type-erased registration, and modern engines (Bevy / many
indie C++ engines) use sidecar / derive-style registration instead.

## Scope

**In scope:**

- `engine/reflection/` module: `TypeId` enum, `FieldDesc` + `FieldMeta` PODs,
  `Reflection` registry class, `TypeIdOf<F>::v` template specializations,
  `Reflection.cpp` with out-of-line bits.
- Host-owned ownership: `iron::Reflection` is a plain object the sandbox
  constructs. Sidecars expose `void register<TypeName>(Reflection&)` free
  functions; host calls each at startup. Matches M37's "host owns the World"
  decision and is robust against static-lib linker pruning.
- Reuse M37's `componentTypeId<T>()` for the per-type integer key (no new
  counter). 256-type cap covers components + reflected non-component types
  comfortably at this scale.
- Sidecar `.reflect.cpp` files for each of the four M37 component types:
  - `engine/world/Transform.reflect.cpp` → `registerTransform(Reflection&)`
  - `engine/scene/MeshRef.reflect.cpp` → `registerMeshRef(Reflection&)`
  - `engine/scene/MaterialDef.reflect.cpp` → `registerMaterialDef(Reflection&)`
  - `engine/render/RenderHandles.reflect.cpp` → `registerRenderHandles(Reflection&)`
- Sandbox startup wiring: `iron::Reflection reflection;` plus four
  `register<TypeName>(reflection)` calls.
- TypeId enum covers v1 primitives:
  - `Bool`, `Int32`, `UInt32`, `UInt8`, `Float`
  - `String` (std::string)
  - `Vec3`, `Quat` (treated atomically — no descent into their floats)
  - `Enum` (any `enum class`)
  - `OptionalEnum` (`std::optional<EnumT>` — covers `MeshRef::primitive`)
  - `Unknown` (sentinel)
- `Reflection` API:
  - `template<class T> TypeBuilder<T> registerType(std::string_view name)`
    — fluent: `.field("position", &T::position)` chains
  - `template<class T> std::span<const FieldDesc> fieldsOf() const`
  - `template<class T> const FieldDesc* fieldByName(std::string_view) const`
  - `template<class T> std::string_view typeName() const`
- `FieldDesc::ptr<T>(void*) -> T*` (and const) for type-erased member access
  via offset.
- New `tests/test_reflection.cpp` — pure logic, no Vulkan, no editor.

**Out of scope (deferred to M39+ or never):**

- **Editor Inspector data-driven field rendering** — M39.
- **SceneIO reflection-driven JSON serialization** (replaces hand-written
  `toJson`/`fromJson`) — M39.
- **Enum name lists** — the `TypeId::Enum` tag carries no "Cube"/"Plane"
  string list. Defer to M39 (Inspector dropdowns need them; the M38 tests
  exercise the tag, not its values).
- **Nested-struct recursion** — `Vec3` stays a primitive. No descent into
  `x/y/z` floats.
- **Inheritance / type-hierarchy metadata** — POD components don't need it.
- **Per-field default values captured at registration time** — consumers
  default-construct `T{}` and compare.
- **The macro sugar layer** (`REFLECT_BEGIN` / `REFLECT_FIELD`) — sidecar
  is the v1 shape. Wrap later if boilerplate bites.
- **Dynamic type registration at runtime** — sidecar functions only run
  once at host startup.
- **Reflection of script-defined types** — M40 (script foundation).
- **AngelScript binding via reflection** — M40.
- **The OO class-with-methods component layer** — M40.
- **Hot-reload of registrations** — static set per process.

## Architecture

### `TypeId` and `FieldDesc`

```cpp
// engine/reflection/TypeId.h
namespace iron {
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

```cpp
// engine/reflection/FieldDesc.h
namespace iron {

struct FieldMeta {
    float min = 0.0f;     // both zero = no clamp/range
    float max = 0.0f;
    // Future: tooltip, hidden, readonly, alias — add when a consumer needs them.
};

struct FieldDesc {
    std::string_view name;
    TypeId           type;
    uint32_t         offset;
    FieldMeta        meta;

    template <class T>
    T* ptr(void* obj) const {
        return reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }
    template <class T>
    const T* ptr(const void* obj) const {
        return reinterpret_cast<const T*>(static_cast<const uint8_t*>(obj) + offset);
    }
};

}  // namespace iron
```

### `TypeIdOf` deduction

```cpp
// engine/reflection/TypeIdOf.h
#include "math/Vec.h"
#include "math/Quaternion.h"
#include "reflection/TypeId.h"

#include <optional>
#include <string>
#include <type_traits>

namespace iron {

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

### `Reflection` registry

```cpp
// engine/reflection/Reflection.h
#include "reflection/FieldDesc.h"
#include "reflection/TypeIdOf.h"
#include "world/Entity.h"   // componentTypeId<T>()

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace iron {

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
            // (static_assert guards in registerType<T>).
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
        // Standard-layout is NOT required — the probe trick works for any
        // addressable non-static data member of a default-constructible T,
        // including types containing std::string / std::optional (which are
        // not standard-layout on MSVC).
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
    const FieldDesc* fieldByName(std::string_view name) const {
        for (const FieldDesc& f : fieldsOf<T>())
            if (f.name == name) return &f;
        return nullptr;
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

(The offset computation uses the null-pointer trick — well-defined for
standard-layout types, which all M37 components are. A guarded assert in
`registerType` can verify via `std::is_standard_layout_v<T>` at registration.)

### Sidecar registration files

```cpp
// engine/world/Transform.reflect.cpp
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

```cpp
// engine/scene/MeshRef.reflect.cpp
#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {
void registerMeshRef(Reflection& r) {
    r.registerType<MeshRef>("MeshRef")
        .field("primitive", &MeshRef::primitive)   // TypeId::OptionalEnum
        .field("gltfPath",  &MeshRef::gltfPath);   // TypeId::String
}
}  // namespace iron
```

```cpp
// engine/scene/MaterialDef.reflect.cpp
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

```cpp
// engine/render/RenderHandles.reflect.cpp
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

### Sidecar declarations header

```cpp
// engine/reflection/RegisterCoreTypes.h — public declarations for all four
namespace iron {
class Reflection;
void registerTransform(Reflection& r);
void registerMeshRef(Reflection& r);
void registerMaterialDef(Reflection& r);
void registerRenderHandles(Reflection& r);
}  // namespace iron
```

### Host wiring

```cpp
// games/11-sandbox/main.cpp — additions near the existing World construction.
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"

iron::Reflection reflection;
iron::registerTransform(reflection);
iron::registerMeshRef(reflection);
iron::registerMaterialDef(reflection);
iron::registerRenderHandles(reflection);
```

The sandbox does not USE the reflection registry in M38 — it just owns and
populates it. M39 picks it up to drive the Inspector and SceneIO.

## File layout

```
engine/reflection/
  TypeId.h                    — TypeId enum (~15 lines)
  FieldDesc.h                 — FieldDesc + FieldMeta (~30 lines)
  TypeIdOf.h                  — TypeIdOf<F>::v specializations (~30 lines)
  Reflection.h                — Reflection class + TypeBuilder<T> (~80 lines)
  Reflection.cpp              — (mostly empty in v1; reserve for out-of-line ctors)
  RegisterCoreTypes.h         — sidecar forward decls (~10 lines)
engine/world/
  Transform.reflect.cpp       — registerTransform (~15 lines)
engine/scene/
  MeshRef.reflect.cpp         — registerMeshRef     (~15 lines)
  MaterialDef.reflect.cpp     — registerMaterialDef (~20 lines)
engine/render/
  RenderHandles.reflect.cpp   — registerRenderHandles (~15 lines)
tests/
  test_reflection.cpp         — ~15-20 unit tests
```

Files compile into `ironcore`. Tests register under `tests/CMakeLists.txt`
with one new `iron_add_test(test_reflection test_reflection.cpp)` line.

## Testing

`tests/test_reflection.cpp` — pure-logic, mirrors `test_world.cpp`'s style
(hand-rolled `CHECK` macro + counted failures + named subtests):

**TypeIdOf deduction:**
- `TypeIdOf<float>::v == TypeId::Float` (and same for every primitive in v1).
- `TypeIdOf<PrimitiveKind>::v == TypeId::Enum` (any enum class).
- `TypeIdOf<std::optional<PrimitiveKind>>::v == TypeId::OptionalEnum`.

**Registry basic operations:**
- After `registerType<Transform>("Transform")`, `typeName<Transform>() ==
  "Transform"`.
- An unregistered type returns an empty `typeName` and an empty `fieldsOf`.
- `fieldsOf<T>()` returns fields in insertion order.

**Field iteration + lookup:**
- `fieldsOf<Transform>().size() == 3` after registration.
- `fieldByName<Transform>("position")` returns a `FieldDesc*` with `type ==
  TypeId::Vec3`, offset matching `offsetof(Transform, position)`.
- `fieldByName<Transform>("nonexistent")` returns `nullptr`.

**Field pointer access:**
- For `Transform t{}; t.position = {1, 2, 3};`,
  `fieldByName<Transform>("position")->ptr<Vec3>(&t) == &t.position`.
- Mutating through `ptr<Vec3>` updates the underlying field.
- The const overload returns `const Vec3*`.

**Field metadata round-trip:**
- `registerType<Transform>("Transform").field("scale", &Transform::scale,
  {.min = 0.001f})` — the resulting `FieldDesc::meta.min == 0.001f` and
  `meta.max == 0.0f`.

**End-to-end with all four M37 components:**
- After running all four `register*(reflection)` calls:
  - `fieldsOf<Transform>().size() == 3`
  - `fieldsOf<MeshRef>().size() == 2` (primitive + gltfPath)
  - `fieldsOf<MaterialDef>().size() == 6`
  - `fieldsOf<RenderHandles>().size() == 4`
- `fieldByName<MeshRef>("primitive")->type == TypeId::OptionalEnum`.
- `fieldByName<MeshRef>("gltfPath")->type == TypeId::String`.
- `fieldByName<MaterialDef>("reflectivity")->meta.min == 0.0f` and
  `meta.max == 1.0f`.

Target: ~15–20 named subtests, single CTest case. Suite 48 → 49.

No new external dependencies. No Vulkan, no editor, no sandbox runtime
needed for any test.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Offset computation via the null-pointer trick is technically UB for non-standard-layout types | Relaxation chosen: the probe uses a *live* default-constructed `T{}` plus pointer arithmetic, which is well-defined for any addressable non-static data member on MSVC/GCC/Clang — including `std::string`/`std::optional`-bearing types that aren't standard-layout on MSVC. `registerType` keeps only `static_assert(std::is_default_constructible_v<T>)`; the SL assert was deliberately dropped. |
| `std::string_view` field names that point into temporaries become dangling | Field names are passed as string literals from sidecar `.cpp` files — static storage duration; lifetime is the whole process. Document the contract: "names must outlive the registry". |
| Sidecar `.cpp` files linked from `ironcore` static lib may be stripped by the linker if no symbol is referenced | The sandbox host explicitly calls each `register<TypeName>(reflection)` function — that forces the linker to keep the TUs. This is exactly why host-owned beats static-init singleton at this stage. |
| `componentTypeId<T>()` reuse means reflected non-component types share the same 256 cap as component types | At M38 scale (4 component types + a few helper structs if needed) the cap is generous. If we ever hit it, lift `kMaxComponentTypes` and `Reflection::kMaxTypes` together. |
| `FieldDesc::ptr<T>(void*)` is unsafe — wrong `T` silently misinterprets memory | This is the standard cost of type-erased access. M39's Inspector and the unit tests both gate by `FieldDesc::type` before calling `ptr<T>`. Document the contract and consider a runtime check in a future polish pass. |
| If M37 generation counter wraps within `componentTypeId<T>()` past 255, the registry index goes out of bounds | Same as M37's known limit; not introduced by M38. M37 spec already noted the cap. |

## Success criteria

1. `iron::Reflection` exists, registers types via the fluent
   `TypeBuilder<T>::field` API.
2. All four M37 component types are registered via sidecar
   `register<TypeName>(Reflection&)` functions, called from the sandbox host
   at startup.
3. `test_reflection.cpp` passes 15–20 subtests covering `TypeIdOf` deduction,
   registry operations, field iteration, by-name lookup, `FieldDesc::ptr`
   access, and end-to-end registration of all four component types.
4. Suite 48 → 49 tests green; sandbox visually indistinguishable from
   M37.5 (M38 is invisible at runtime).
5. Zero new external dependencies. Renderer, editor panels, picking, gizmo,
   SceneIO, World — all untouched.
6. The sandbox creates and populates the `Reflection` registry but doesn't
   consume it yet. M39 picks it up.

After M38: foundation track moves to **M39** (editor + serialization migrate
onto reflection — Inspector becomes data-driven, SceneIO's hand-written
`toJson`/`fromJson` is replaced with a generic reflection-driven encoder).
