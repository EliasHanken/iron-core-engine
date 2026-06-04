## Summary

M39 — third foundation-track milestone. Consumes M38's `iron::Reflection` in two places: the editor Inspector renders fields from `fieldsOf<T>()` via a TypeId dispatch, and `SceneIO` serializes entity components through the same registry. Unifies `SceneEntity` with M37's `Transform` (drops the duplicated bare `position` / `rotation` / `scale` fields). Adds an `EnumBuilder` so dropdowns and JSON enum strings are registry-driven.

- **New module `engine/editor/ReflectionInspector.{h,cpp}`**: generic `renderComponent<T>` template + ImGui `TypeId` dispatch (Bool / Int* / Float / String / Vec3 / Quat / Enum / OptionalEnum). Widget hints (color, slider, dragSpeed) consulted via `FieldMeta`. Quat fields rendered as euler-decomposed DragFloat3.
- **New module `engine/scene/ReflectionIO.{h,cpp}`**: symmetric `componentToJson<T>` / `componentFromJson<T>`. Empty strings omitted; nullopt enums omitted; unknown enum names log + fall back to the first registered value. Single-lookup pattern via `j.find()`.
- **Reflection schema extension**: `FieldMeta` gains `dragSpeed` / `color` / `slider`. `FieldDesc` gains `enumTypeId` (populated by `TypeBuilder::field` via a concept-constrained `enumTypeIdOf<F>()` helper, so dispatch can look up enum value lists without knowing the concrete `E`). `Reflection` grows `EnumBuilder<E>`, `registerEnum<E>`, `enumValues<E>` / `enumName<E>`, and non-template `enumValuesById` / `enumNameById`. EnumBuilder carries a `static_assert(sizeof(underlying_type_t<E>) == sizeof(int32_t))` so a future enum with a different underlying type fails at compile time instead of silently misaligning at runtime.
- **`SceneEntity` refactor**: `Transform transform` member replaces bare `position` / `rotation` / `scale`. Mechanical `e.position` → `e.transform.position` rename across `SceneInspector` / `Gizmo` / `SceneIO` / sandbox / tests. The M37 Inspector → World sync collapses from a three-field copy to a single struct assignment `*t = se.transform`.
- **Sidecar updates**: `MeshRef.reflect.cpp` registers `PrimitiveKind` enum (`"cube"` / `"plane"`); `MaterialDef.reflect.cpp` carries `.color = true` on emissive and `.slider = true` on reflectivity.
- **Editor UX bonus**: `MeshRef::gltfPath` is now editable (was display-only); primitive shows a dropdown with a "None" entry.
- **On-disk format change**: scene JSON nests `transform` / `mesh` / `material` per entity. **No backward-compat fallback** — `games/11-sandbox/assets/scenes/demo.json` hand-migrated as part of M39 (the engine is pre-1.0; cleaner break, no lingering compat shim).
- **Tests**: new `tests/test_reflection_io.cpp` (9 named subtests — Transform/MeshRef/MaterialDef roundtrips, enum present/none, RenderHandles uint32 path, hint isolation, no-load-clamp, unknown-enum fallback). `tests/test_scene_io.cpp` ported to the new nested format with a new "save emits nested transform" assertion. 49 → 50 tests.

## Test plan

- [x] Full suite green (50/50)
- [x] ironcore + ironcore_editor + sandbox build clean
- [x] Visual: Inspector renders Transform / MeshRef / MaterialDef from reflection; mesh primitive dropdown; gltfPath editable; emissive ColorEdit3; reflectivity slider; save → reload roundtrip on migrated demo.json

## Known v1 limitations (intentional, deferred)

- World migration (editor reads `iron::World` directly; `resolved[]` deleted; sync hack removed) — separate milestone.
- Nested-struct reflection (`SceneEntity` itself reflectable) — defer.
- `sun` / `fog` / `pointLights` / `clearColor` through reflection — possible follow-up.
- `readonly` / `hidden` / `tooltip` / `category` field flags — YAGNI.
- Enum underlying-type fixed at int32 in the dispatch (guarded by `EnumBuilder::value`'s `static_assert`). Future enums with `: uint8_t` etc. would need a templated dispatch.
- Macro sugar (`REFLECT_BEGIN`) — sidecar pattern is v1.
- Reflection of script-defined types + AngelScript binding — M40+.
- Class-with-methods OO component layer — M40+.
