# M39 — Editor Inspector + SceneIO migrate onto `iron::Reflection`

**Status:** approved 2026-05-31 (this branch)
**Predecessor:** M38 — reflection / type registry (PR #59, `5e5af2c`)
**Successor (proposed):** M40 — World migration (editor reads `iron::World` directly; `resolved[]` deleted; sync hack removed)

## Goal

Make the editor Inspector render data-driven from `iron::Reflection`, and make `SceneIO` serialize through the same registry. Pay the M38 dividend: adding a new component no longer means editing the Inspector and SceneIO in two places — just registering the type with hints and the editor + JSON pick it up.

Refactor `SceneEntity` so its `position`/`rotation`/`scale` fields become a single `Transform transform` member. This removes the duplication M37 introduced (Transform on the World side, bare fields on the SceneEntity side), aligns the editor-facing aggregate with the M37 component model, and lets the Inspector body collapse to a uniform loop over `{transform, mesh, material}`.

## Non-goals (explicit deferrals)

- **World migration.** The editor still operates on `SceneFile` / `SceneEntity`. The end-of-frame `Inspector → World` sync hack from M37 stays. `resolved[]` survives. This is its own milestone with its own brainstorm (generational-id roundtrips in JSON, ECS-as-source-of-truth implications, undo/redo positioning).
- **Nested-struct reflection.** `SceneEntity` itself is not registered; SceneIO/Inspector keep a hand-rolled outer loop that knows the aggregate's shape. The registered things are the *components*.
- **`sun` / `fog` / `pointLights` / `clearColor` through reflection.** Out of scope for M39; potential follow-up.
- **`readonly` / `hidden` / `tooltip` / `category` field flags** — YAGNI. Add when needed.
- **Macro sugar** (`REFLECT_BEGIN` / `REFLECT_FIELD`) — sidecar pattern is v1.
- **Reflection of AngelScript-defined types** — M40+.
- **Class-with-methods OO component layer** — M40+ alongside AngelScript.

## Architecture

### Schema additions

**`FieldMeta` gains three fields** (engine/reflection/FieldDesc.h):

```cpp
struct FieldMeta {
    float min       = 0.0f;
    float max       = 0.0f;
    float dragSpeed = 0.0f;  // 0 = TypeId default
    bool  color     = false; // Vec3 → ColorEdit3 instead of DragFloat3
    bool  slider    = false; // float → SliderFloat instead of DragFloat (needs min+max)
};
```

Designated init at sidecar registrations stays readable:
```cpp
.field("emissive",     &MaterialDef::emissive,     {.color = true})
.field("reflectivity", &MaterialDef::reflectivity, {.min = 0, .max = 1, .slider = true})
```

Convention rules for the Inspector dispatch:
- `dragSpeed == 0` → TypeId default: `Vec3 = 0.05f`, `Quat = 0.5f`, `Float = 0.05f`
- `slider == true` requires both `min` and `max` set; otherwise dispatch falls back to clamped drag and logs a warning at registration time
- `color == true` is honored for `Vec3` only; ignored on other types (logged at registration)

**`FieldDesc` gains one field — `uint32_t enumTypeId = 0`** — populated by `TypeBuilder::field` via a concept-constrained `enumTypeIdOf<F>()` helper:

```cpp
template <class F>
constexpr uint32_t enumTypeIdOf() {
    if constexpr (std::is_enum_v<F>)
        return componentTypeId<F>();
    else if constexpr (is_optional_enum_v<F>)
        return componentTypeId<typename F::value_type>();
    else
        return 0;
}
```

Where `is_optional_enum_v<F>` detects `std::optional<E>` with `std::is_enum_v<E>`. Zero means "not an enum field"; non-zero is the registry id of the enum's type entry, used by dispatch to look up value names without knowing the concrete `E`.

### Reflection enum registration

```cpp
struct EnumValue {
    std::string_view name;
    int64_t          value;
};

template <class E>
class EnumBuilder {
public:
    EnumBuilder& value(std::string_view name, E v);
private:
    friend class Reflection;
    EnumBuilder(Reflection& r, uint32_t id);
    Reflection& reg_;
    uint32_t    typeId_;
};

class Reflection {
    // ... existing M38 surface ...

    template <class E>
    EnumBuilder<E> registerEnum(std::string_view name);

    template <class E>
    std::span<const EnumValue> enumValues() const;

    template <class E>
    std::string_view enumName() const;

    // Dispatch-side lookup — Inspector/SceneIO don't know E.
    std::span<const EnumValue> enumValuesById(uint32_t id) const;
    std::string_view           enumNameById(uint32_t id) const;
};
```

Storage: parallel `std::array<EnumEntry, kMaxTypes> enums_` and `std::array<bool, kMaxTypes> enumsRegistered_`, keyed by the same `componentTypeId<E>()` id space as types. `EnumEntry { std::string_view name; std::vector<EnumValue> values; }`.

`int64_t` for `EnumValue::value` covers all enum underlying types (signed and unsigned, up to 64-bit) via `static_cast`. Dispatch reads via `static_cast<E>(static_cast<UnderlyingType>(value))`.

**Conventions:**
1. **Names lowercase to match the on-disk JSON format.** Inspector dropdown labels = JSON strings. Greppable. Same pattern Unreal uses for DisplayName.
2. **Enum registration lives in the sidecar of the type that owns the enum.** `PrimitiveKind` is declared in `SceneFormat.h`, so its registration goes in `MeshRef.reflect.cpp` (which already includes `SceneFormat.h`). No new file.
3. **`std::optional<E>` handled in the dispatch layer.** Inspector prepends "None" to the value list and resets the optional when selected. SceneIO writes the value name or omits the field on `nullopt`. Reflection itself doesn't know about optional.

### Component model refactor

`SceneEntity` contains a `Transform` instead of bare fields:

```cpp
// engine/scene/SceneFormat.h — before
struct SceneEntity {
    std::string name;
    Vec3        position = {0,0,0};
    Quat        rotation = Quat::identity();
    Vec3        scale    = {1,1,1};
    MeshRef     mesh;
    MaterialDef material;
};

// after
struct SceneEntity {
    std::string name;
    Transform   transform;       // engine/world/Transform.h
    MeshRef     mesh;
    MaterialDef material;
};
```

**Call-site impact** (verified via grep): ~25 occurrences across 5 files. Mechanical `e.position` → `e.transform.position` etc.
- `engine/editor/SceneInspector.cpp` ×4
- `engine/editor/Gizmo.cpp` ×10
- `engine/scene/SceneIO.cpp` ×6
- `games/11-sandbox/main.cpp` ×~3
- `engine/editor/EnvironmentPanel.cpp` ×1

The M37 Inspector → World sync hack *simplifies*: previously was field-by-field copy of three Vec3/Quat values from `scene.entities[i]` into `world.get<Transform>(eid)`. Becomes a single struct assignment `world.get<Transform>(eid) = e.transform;`.

### Updated sidecar registrations

`Transform.reflect.cpp` — unchanged from M38 (already has the right field list and the `scale.min = 0.001f` clamp).

`MaterialDef.reflect.cpp` — add widget hints:
```cpp
r.registerType<MaterialDef>("MaterialDef")
    .field("albedoPath",   &MaterialDef::albedoPath)
    .field("normalPath",   &MaterialDef::normalPath)
    .field("specularPath", &MaterialDef::specularPath)
    .field("emissive",     &MaterialDef::emissive,     {.color = true})
    .field("uvScale",      &MaterialDef::uvScale,      {.min = 0.0f, .max = 100.0f})
    .field("reflectivity", &MaterialDef::reflectivity, {.min = 0, .max = 1, .slider = true});
```

`MeshRef.reflect.cpp` — add enum registration before the type registration:
```cpp
void registerMeshRef(Reflection& r) {
    r.registerEnum<PrimitiveKind>("PrimitiveKind")
        .value("cube",  PrimitiveKind::Cube)
        .value("plane", PrimitiveKind::Plane);
    r.registerType<MeshRef>("MeshRef")
        .field("primitive", &MeshRef::primitive)   // FieldDesc.enumTypeId auto-set
        .field("gltfPath",  &MeshRef::gltfPath);
}
```

`RenderHandles.reflect.cpp` — unchanged from M38. Stays registered for symmetry, never rendered in Inspector (runtime-only GPU handles; the Inspector loop only iterates the three user-facing components).

### `engine/editor/ReflectionInspector.{h,cpp}` (NEW)

Generic helper `bool renderComponent<T>(const Reflection&, T&)` that walks `fieldsOf<T>()` and dispatches per `TypeId`. Returns `true` if any field changed this frame. Section header rendered as `ImGui::SeparatorText(reflection.typeName<T>().data())`.

Dispatch table:

| TypeId | Widget | Notes |
|---|---|---|
| `Bool` | `ImGui::Checkbox` | name from field |
| `Int32`, `UInt32`, `UInt8` | `ImGui::InputInt` (or DragInt) | clamp if min/max set; UInt32/UInt8 cast |
| `Float` | `SliderFloat` if `meta.slider`, else `DragFloat` (clamped if min/max set; speed = `meta.dragSpeed` or 0.05) | |
| `String` | `ImGui::InputText` (resizable callback over `std::string`) | |
| `Vec3` | `ColorEdit3` if `meta.color`, else `DragFloat3` (speed = `meta.dragSpeed` or 0.05) | |
| `Quat` | Euler-decompose to `DragFloat3` (degrees), recompose on edit via `eulerToQuat`. Speed = `meta.dragSpeed` or 0.5. | universal — no per-field hint needed |
| `Enum` | `Combo` over `enumValuesById(f.enumTypeId)` | linear search for current value's index; if no match, set display index to 0, log a one-shot warning, do not write back until user picks a value |
| `OptionalEnum` | "None" + Combo. Selecting "None" → `.reset()`. Selecting a value → assign. | "None" is index 0; enum values start at index 1 |
| `Unknown` | render label only ("(unknown type)") | should never happen — registration-time bug if it does |

Width of value column controlled by `ImGui::PushItemWidth(-FLT_MIN)` or similar to fit the panel layout.

**Header sketch:**
```cpp
// engine/editor/ReflectionInspector.h
#pragma once
#include "reflection/Reflection.h"
namespace iron {
template <class T>
bool renderComponent(const Reflection& r, T& obj);
}  // namespace iron
```

Implementation in `.cpp` instantiates the four user-facing types explicitly (Transform, MeshRef, MaterialDef — plus RenderHandles for symmetry / future use) using a single private `renderFieldsByPtr(r, typeName, fields, void*)` worker, so the template only forwards `&obj` + `fieldsOf<T>()` + `typeName<T>()`.

### `engine/scene/ReflectionIO.{h,cpp}` (NEW)

Symmetric reflection-driven JSON converters:

```cpp
// engine/scene/ReflectionIO.h
#pragma once
#include "reflection/Reflection.h"
#include <nlohmann/json.hpp>
namespace iron {
template <class T>
nlohmann::json componentToJson(const Reflection& r, const T& obj);
template <class T>
void componentFromJson(const Reflection& r, T& obj, const nlohmann::json& j);
}  // namespace iron
```

Serialization rules per TypeId:
- `Bool` → JSON bool
- `Int32` / `UInt32` / `UInt8` → JSON number
- `Float` → JSON number
- `String` → JSON string (omitted on write if empty; on read, missing key leaves default)
- `Vec3` → 3-element array (color flag does NOT affect serialization)
- `Quat` → 4-element array `[x, y, z, w]` (matches existing SceneIO convention)
- `Enum` → JSON string (the registered value name); falls back to `componentTypeId<E>`'s first value if name not matched on read
- `OptionalEnum` → JSON string when present; omitted entirely when `nullopt`. On read, missing key → `nullopt`.

Min/max/slider/color/dragSpeed hints are not serialized — they're UI hints. SceneIO ignores them.

Implementation mirrors ReflectionInspector: a private `componentToJsonByPtr` + `componentFromJsonByPtr` worker takes the void* + field list + (for `to`) writes into the JSON object; templates forward.

### Updated `SceneInspector`

```cpp
bool SceneInspector::draw(const Reflection& r, SceneEntity& e,
                          GizmoSpace& space, EffectKind& effectKind) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Editor-tool prelude (NOT entity data — stays hand-rolled).
    renderGizmoSpaceToggle(space);
    renderSelectionEffectPicker(effectKind);

    // Entity body — purely reflection-driven.
    changed |= renderComponent(r, e.transform);
    changed |= renderComponent(r, e.mesh);
    changed |= renderComponent(r, e.material);

    ImGui::End();
    return changed;
}
```

Gizmo-space toggle and selection-effect picker stay because they're editor *tool* state, not entity data. They live in the panel's prelude.

The "(read-only)" mesh display goes away — `MeshRef::primitive` becomes an editable enum dropdown and `MeshRef::gltfPath` becomes an editable `InputText`. (Live mesh swap is a runtime concern handled by the existing handle resolver in the sandbox; the Inspector just sets the strings/enum.)

**Caller** (`games/11-sandbox/main.cpp`) updates: `inspector.draw(reflection, scene.entities[sel], gizmoSpace, effectKind)`.

### Updated `SceneIO`

`entityToJson`:
```cpp
json entityToJson(const Reflection& r, const SceneEntity& e) {
    json j = json::object();
    j["name"]      = e.name;
    j["transform"] = componentToJson(r, e.transform);
    j["mesh"]      = componentToJson(r, e.mesh);
    j["material"]  = componentToJson(r, e.material);
    return j;
}
```

`entityFromJson` — legacy-flat backward compatibility for one release:
```cpp
SceneEntity entityFromJson(const Reflection& r, const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);

    // Legacy: flat position/rotation/scale at entity level.
    if (j.contains("position") || j.contains("rotation") || j.contains("scale")) {
        readVec3(j, "position", e.transform.position);
        readQuat(j, "rotation", e.transform.rotation);
        readVec3(j, "scale",    e.transform.scale);
        Log::info("SceneIO: loaded entity '%s' from legacy flat transform format",
                  e.name.c_str());
    }
    // Preferred: nested.
    if (j.contains("transform")) componentFromJson(r, e.transform, j["transform"]);
    if (j.contains("mesh"))      componentFromJson(r, e.mesh,      j["mesh"]);
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);
    return e;
}
```

Save always emits nested. Legacy fallback to be removed in M40+.

`saveSceneFile` / `loadSceneFile` take an additional `const Reflection&` parameter. Sun / fog / point lights / clearColor stay hand-rolled (not registered in M38; out of scope for M39).

## Tests

**New file: `tests/test_reflection_io.cpp`** (~10 named subtests):
- `test_component_to_json_transform_roundtrip` — register, mutate, serialize, parse, deserialize, compare
- `test_component_to_json_mesh_ref_roundtrip_primitive_some` — primitive = Cube
- `test_component_to_json_mesh_ref_roundtrip_primitive_none` — primitive = nullopt, omitted from JSON
- `test_component_to_json_material_def_roundtrip` — all 6 fields incl. emissive Vec3
- `test_enum_unknown_name_falls_back_to_first_value` — robustness on hand-edited JSON
- `test_widget_hints_do_not_affect_serialization` — emissive with color=true serializes same as a non-color Vec3
- `test_min_max_do_not_clamp_on_load` — load a value outside [min, max], confirm stored as-is
- `test_optional_enum_omits_on_nullopt` — JSON output does NOT contain the key

**Updated file: `tests/test_scene_io.cpp`** — keep existing roundtrip but use the new nested format. Add:
- `test_scene_io_loads_legacy_flat_transform_format` — hand-authored old JSON loads correctly
- `test_scene_io_save_emits_nested_transform_format` — saved JSON contains `"transform": {...}` and NOT top-level `"position"`

49 → 50 total CTest cases (one new file).

The Inspector dispatch is visual-gated (no ImGui in unit tests), same as the current `SceneInspector`. The integration test is the sandbox visual gate at Phase D.

## File map

**New:**
- `engine/editor/ReflectionInspector.h`
- `engine/editor/ReflectionInspector.cpp`
- `engine/scene/ReflectionIO.h`
- `engine/scene/ReflectionIO.cpp`
- `tests/test_reflection_io.cpp`

**Modified:**
- `engine/reflection/FieldDesc.h` — add `dragSpeed`/`color`/`slider` to `FieldMeta`; add `enumTypeId` to `FieldDesc`
- `engine/reflection/TypeIdOf.h` — add `is_optional_enum_v` + `enumTypeIdOf<F>()` helpers
- `engine/reflection/Reflection.h` — `TypeBuilder::field` populates `enumTypeId`; add `EnumBuilder<E>` + `registerEnum<E>` + `enumValues<E>` + `enumName<E>` + non-template `enumValuesById` / `enumNameById`
- `engine/scene/SceneFormat.h` — `SceneEntity` contains `Transform transform`; drop bare position/rotation/scale; `#include "world/Transform.h"`
- `engine/scene/SceneIO.h` — `saveSceneFile` / `loadSceneFile` take `const Reflection&`
- `engine/scene/SceneIO.cpp` — call `componentToJson` / `componentFromJson` for each entity's three components; legacy fallback for flat top-level pos/rot/scale
- `engine/scene/MeshRef.reflect.cpp` — add `registerEnum<PrimitiveKind>`
- `engine/scene/MaterialDef.reflect.cpp` — add `.color = true` and `.slider = true` hints
- `engine/editor/SceneInspector.h` — `draw` takes `const Reflection&`
- `engine/editor/SceneInspector.cpp` — invoke `renderComponent` for the 3 user-facing components; keep editor-tool prelude
- `engine/editor/Gizmo.cpp` — `e.position` / `e.rotation` / `e.scale` → `e.transform.*` (10 occurrences)
- `engine/editor/EnvironmentPanel.cpp` — single occurrence rename if applicable
- `engine/CMakeLists.txt` — add `editor/ReflectionInspector.cpp` + `scene/ReflectionIO.cpp` to `ironcore` source list
- `tests/CMakeLists.txt` — `iron_add_test(test_reflection_io test_reflection_io.cpp)`
- `tests/test_scene_io.cpp` — nested format + legacy load tests
- `games/11-sandbox/main.cpp` — pass `reflection` to `inspector.draw` and `loadSceneFile` / `saveSceneFile`; transform field rename in any direct accesses; simplify Inspector → World sync to a struct assignment

**Untouched on purpose:** Renderer (Vulkan), World / ComponentArray, picking, Outliner (its add/delete/duplicate paths only mutate `scene.entities` — the SceneEntity reshape is transparent to it), shipping games (net-shooter, 02-strandbound, 04-net-pingpong, 05-net-cubes, 06-net-tag, etc.), all of the M16/M17 reflection (the misnamed-overlap is unfortunate but harmless).

## Phases (for the plan)

- **A — Schema extension.** FieldMeta + FieldDesc growth, EnumBuilder + registerEnum + enumValuesById, TypeIdOf enum-id helpers. Pure logic, TDD. ~3 tasks.
- **B — Component model refactor.** `SceneEntity` contains `Transform`. Mechanical renames + legacy fallback in SceneIO. Build clean + existing tests green. ~1 task.
- **C — Reflection-driven helpers.** ReflectionIO + ReflectionInspector + sidecar widget hints + enum registration. ReflectionIO tested via test_reflection_io; ReflectionInspector exercised by sandbox. ~3 tasks.
- **D — Wire-up.** SceneInspector body, SceneIO entity ser/deser, sandbox main calls. ~1 task.
- **E — Visual gate + PR + merge + memory.** Full sandbox sweep, push, PR, squash-merge, memory update. ~1 task.

Roughly 9 tasks. M38-comparable scope.

## Acceptance criteria

1. Inspector renders Transform, MeshRef, MaterialDef via `renderComponent<T>` — no hand-rolled per-field widgets remain in the component body (editor-tool prelude excepted).
2. `MeshRef::primitive` is a dropdown showing "None / cube / plane"; "None" clears the optional.
3. `MeshRef::gltfPath` is now editable as a text field (was display-only).
4. `MaterialDef::emissive` renders as `ColorEdit3`.
5. `MaterialDef::reflectivity` renders as `SliderFloat` 0..1.
6. `SceneIO::saveSceneFile` / `loadSceneFile` roundtrip through `componentToJson` / `componentFromJson`.
7. Legacy flat scene files (position / rotation / scale at entity level) still load — read into `e.transform.*`.
8. New saves emit nested `"transform": {...}` and do NOT include top-level position/rotation/scale keys.
9. `SceneEntity` no longer has bare position/rotation/scale fields.
10. All four editor flows still work: click-select, gizmo drag (translate / rotate / scale), add/delete/duplicate, save/load.
11. 50 / 50 tests green.
12. Sandbox visually indistinguishable from M38 (M39 is a behavior-preserving refactor + minor editing-affordance expansion: mesh fields now editable).
13. Renderer / picking / shipping games untouched.

## Known robustness concerns (NOT addressed by M39)

- `componentTypeId<T>()` is a function-local static counter (M37 follow-up). With strict ODR enforcement across debug build flavors and translation-unit ordering, two `.cpp` files could theoretically observe different ids. M39 increases the surface area that depends on cross-TU id consistency (enum registration in sidecar `.reflect.cpp`, dispatch lookup in `ReflectionInspector.cpp` / `ReflectionIO.cpp`), but the function-local-static pattern has worked across M37 / M38 in practice. M39 watches for symptoms; the proper fix (move the counter to a `.cpp` with an extern declaration) stays the M37 follow-up it already was.

## Open questions (resolved in brainstorm; restated for clarity)

- *Per-field widget hints inline in FieldMeta or via a sub-struct?* — inline. Designated init reads better; M38 set the precedent.
- *Enum registration where?* — fluent `registerEnum<E>` on the registry, in the sidecar of the type that owns the enum. Names lowercase to match on-disk format.
- *Optional-enum dispatch?* — Inspector / SceneIO layer handles the `optional` wrapper; reflection only exposes enum values.
- *Refactor SceneEntity to contain Transform?* — yes. The duplication is the cost of M37 stopping at the World layer; M39 fixes it.
- *Drag speed defaults?* — `dragSpeed == 0` → TypeId default in the Inspector dispatch (Vec3 = 0.05, Quat = 0.5, Float = 0.05). Override only when needed.
- *Slider vs drag?* — explicit `meta.slider = true` (avoids ambiguity with clamped drags that also set min+max).
- *Quat editing?* — universal euler-decompose at the dispatch layer. No per-field hint required.
- *RenderHandles in Inspector?* — no. The Inspector loop only iterates the three user-facing components. RenderHandles stays registered for symmetry / future use.
