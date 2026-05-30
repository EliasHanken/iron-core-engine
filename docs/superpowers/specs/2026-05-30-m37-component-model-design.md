# M37 — Component model: World + first three POD components (Design)

**Date:** 2026-05-30
**Milestone:** M37 (foundation track — first of A: components → reflection → editor integration → scripting → port systems)
**Prerequisite:** M29 scene serialization (`SceneFile` / `SceneEntity`); M30+ editor module (`SceneInspector` / `SceneOutliner` / `Gizmo`); `games/11-sandbox` editor host.

## Goal

Break the fixed-struct ceiling of `SceneEntity`. Build an entity-component
runtime container (`iron::World`) and port the existing renderable-entity
data (`Transform` + `MeshRef` + `MaterialDef` + resolved GPU handles) into
component form. After M37, every renderable entity in the sandbox flows
through the `World`; the renderer reads from it; the on-disk format and the
editor UI still operate the M29 way for now.

This is the **structural unlock** for everything else in the foundation
track — reflection (M38), editor component add/remove (M39), AngelScript
text scripting (M40), and the per-system ports (M41+ : physics body,
character controller, weapon, audio emitter, etc.). None of those can land
cleanly until the World exists.

## Context: how Unreal / Unity do this (why this shape)

Unreal: an `AActor` owns a list of `UActorComponent`s. A renderable actor has
a `UStaticMeshComponent` that holds a `UStaticMesh*` (the asset reference);
the mesh's GPU resources are managed by the asset. *Everything about the
actor lives on the actor.* No parallel side-tables outside the world.

Unity: a `GameObject` owns a list of `Component`s. A renderable game object
has a `MeshFilter` (mesh reference) + `MeshRenderer` (material array). Same
principle.

The iron-core-engine current shape diverges from this — `SceneEntity` is a
fixed struct, the resolved GPU handles live in a host-side `resolved[]`
vector keyed by integer index, and the renderer crossreferences both. M37
collapses that into the standard pattern: **the World owns the entity, the
entity owns its components, and the components include the runtime
references.** See [[feedback-prefer-unreal-patterns]].

## Scope

**In scope:**
- `iron::World` runtime container — single-threaded, host-owned, no
  scheduling.
- `EntityId` (32-bit index + 32-bit generation; generation 0 = `kEntityNone`).
- `ComponentArray<T>` per-type storage — dense `vector<T>` + sparse
  `vector<uint32_t>` index from entity row → dense row. O(1) add / get /
  remove via swap-and-pop.
- Three POD component types ported as the v1 proof: `Transform`,
  `MeshRef` (reused from `SceneFormat.h`), `MaterialDef` (reused).
- `RenderHandles { MeshHandle; TextureHandle albedo, normal, specular; }`
  — runtime asset-handle component, sibling of `MeshRef`/`MaterialDef`.
- `SceneIO::load` decomposes each `SceneEntity` into the four components on
  the World. The existing `resolveEntity` is refactored to fill a
  `RenderHandles` instead of pushing to a parallel cache.
- The sandbox's per-frame submit loop iterates the World instead of
  `resolved[]`. Renderer code unchanged.
- End-of-frame Inspector → World sync: after the editor panels run, copy
  `scene.entities[i].position/rotation/scale/material` into the matching
  World entity's components. One short loop in `games/11-sandbox/main.cpp`.
- A `sceneIndex → EntityId` table maintained by the loader so the editor's
  existing `int selectedIndex` continues to address World entities.

**Out of scope:**
- The hybrid design's **OO half** — class-with-methods user components.
  Deferred to M40 alongside AngelScript (no consumer for it before then).
- **Reflection / type registry** — M38.
- **Editor add/remove arbitrary components** — M39 (needs reflection).
- **Generic component JSON serialization** — M39.
- **System scheduling, dependency graph, parallelism** — single-threaded
  fixed order forever for v1.
- **Multi-component queries** (`world.view<A, B>()`) — v1 ships
  `world.view<T>()` for single-type iteration plus per-entity
  `world.get<U>(e)` for cross-component reads.
- **Archetype storage** — dense vec + sparse index is fine at TF2/Overwatch
  entity scale (~50–200 entities).
- **Multi-world, snapshots, replication** — single World per host.
- **Migrating Outliner / Inspector / Gizmo / picking off `SceneFile`** —
  they keep operating on `scene.entities[]` with the end-of-frame
  Inspector → World sync. Migration lands in M38–M40 once reflection and
  generic serialization exist.
- **Selecting by `EntityId`** — `selectedIndex` stays `int`; the
  `sceneIndex → EntityId` table bridges to the World.

## Architecture

### `EntityId`

```cpp
// engine/world/Entity.h
struct EntityId {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const EntityId&) const = default;
};
inline constexpr EntityId kEntityNone{};
```

Generation 0 = "no entity" — every real entity carries generation ≥ 1.
On `destroy(e)`, the slot's generation is bumped; a recycled slot returns
a *new* `EntityId` whose generation differs. Stale references that survive
a destroy are caught by `alive(e)` (false → ignore).

### `ComponentArray<T>`

```cpp
// engine/world/ComponentArray.h
template <class T>
class ComponentArray {
public:
    T*       add(EntityId, const T& = {});
    T*       get(EntityId);
    const T* get(EntityId) const;
    void     remove(EntityId);
    size_t   size() const;
    T&       operator[](size_t denseRow);
    const T& operator[](size_t denseRow) const;
    EntityId entityAt(size_t denseRow) const;

private:
    std::vector<T>        dense_;
    std::vector<EntityId> denseEntities_;
    std::vector<uint32_t> sparse_;   // sparse_[e.index] = dense row, or kNoRow
    static constexpr uint32_t kNoRow = UINT32_MAX;
};
```

Add: append to `dense_`, write the dense row into `sparse_[e.index]`.
Get: look up `sparse_[e.index]`, return `&dense_[row]` (or nullptr).
Remove: swap-and-pop the dense entry; update the swapped entity's
`sparse_[].` Iteration walks `dense_` contiguously.

### `World`

```cpp
// engine/world/World.h
class World {
public:
    EntityId create();
    void     destroy(EntityId);
    bool     alive(EntityId) const;

    template <class T> T*       add(EntityId, const T& = {});
    template <class T> T*       get(EntityId);
    template <class T> const T* get(EntityId) const;
    template <class T> void     remove(EntityId);

    template <class T> ComponentArray<T>&       view();
    template <class T> const ComponentArray<T>& view() const;

private:
    // TypeId (compile-time small int per T) -> unique_ptr<IComponentArray>
    std::array<std::unique_ptr<IComponentArray>, kMaxComponentTypes> arrays_;
    std::vector<uint32_t> generations_;  // per slot
    std::vector<uint32_t> freeList_;
};
```

`componentTypeId<T>()` returns a stable small integer per type via the
counter-template pattern. `kMaxComponentTypes = 256` for v1.

`destroy(e)` walks every active `ComponentArray` and removes the entity.
This is **the load-bearing reason GPU handles live in the World** — a
sandbox-side parallel cache would need to manually remember to delete the
handle on every destroy.

### Component types

- `Transform` (new, `engine/world/Transform.h`):
  ```cpp
  struct Transform {
      Vec3 position = {0, 0, 0};
      Quat rotation = Quat::identity();
      Vec3 scale    = {1, 1, 1};
  };
  ```
- `MeshRef` — reused from `engine/scene/SceneFormat.h`. No wrapper.
- `MaterialDef` — reused from `engine/scene/SceneFormat.h`. No wrapper.
- `RenderHandles` (new, `engine/render/RenderHandles.h`):
  ```cpp
  struct RenderHandles {
      MeshHandle    mesh;
      TextureHandle albedo;
      TextureHandle normal;
      TextureHandle specular;
  };
  ```

`MeshRef` and `MaterialDef` are the **authored** data (paths, primitives,
emissive, uvScale, reflectivity). `RenderHandles` is the **runtime asset
reference** for the same entity. Two sibling components, one for the
content author, one for the GPU.

## Data flow

### Load

```
demo.json
  → SceneIO::load → SceneFile                                  (unchanged)
  → for each (sceneIdx, SceneEntity& se) in scene.entities:
        EntityId e = world.create();
        world.add<Transform>   (e, {se.position, se.rotation, se.scale});
        world.add<MeshRef>     (e, se.mesh);
        world.add<MaterialDef> (e, se.material);
        RenderHandles rh = resolveEntity(se, renderer);
        world.add<RenderHandles>(e, rh);
        sceneIndexToEntity_.push_back(e);   // sceneIdx → e
```

`resolveEntity` is the existing function — refactored to **return** a
`RenderHandles` instead of writing a `ResolvedEntity`. The host-side
`resolved[]` vector is **deleted**.

### Render submit

```cpp
auto& transforms = world.view<Transform>();
for (size_t row = 0; row < transforms.size(); ++row) {
    EntityId e = transforms.entityAt(row);
    const Transform&     t   = transforms[row];
    const MeshRef*       mr  = world.get<MeshRef>(e);
    const MaterialDef*   mat = world.get<MaterialDef>(e);
    const RenderHandles* rh  = world.get<RenderHandles>(e);
    if (!mr || !mat || !rh) continue;
    DrawCall dc = buildDrawCall(t, *mat, *rh);  // existing helper, adapted
    renderer.submit(dc, view, proj);
}
```

The renderer code (Vulkan backend) is **untouched**. Only the sandbox
submit loop changes.

### Inspector → World end-of-frame sync

The Inspector still edits `scene.entities[selectedIndex]` directly. At the
end of the editor frame, before render submit:

```cpp
for (size_t i = 0; i < scene.entities.size(); ++i) {
    const SceneEntity& se = scene.entities[i];
    EntityId e = sceneIndexToEntity_[i];
    if (auto* t = world.get<Transform>(e)) {
        t->position = se.position;
        t->rotation = se.rotation;
        t->scale    = se.scale;
    }
    if (auto* m = world.get<MaterialDef>(e)) *m = se.material;
    // MeshRef does not change at runtime; skip.
}
```

Cheap (a handful of pointer chases per entity) at the entity scale we
target. Replaced in M39 once the Inspector edits the World directly.

### Entity destroy

`world.destroy(e)` tears off all four components. The sandbox additionally
removes `e` from `sceneIndexToEntity_` and reindexes (mirrors the current
`scene.entities.erase()` + resolved-reindex logic).

## Editor / gizmo / picking — no behavioral change in M37

| Subsystem | Operates on | When does it migrate? |
|---|---|---|
| `SceneOutliner` | `scene.entities[]` (int) | M39 |
| `SceneInspector` | `scene.entities[selectedIndex]` | M39 |
| `Gizmo` | `scene.entities[selectedIndex]` | M39 |
| Picking | `scene.entities[]` ray vs. AABB | M39 (small — moves with Inspector) |
| Render submit | **World (new)** | M37 — this milestone |

## File layout

```
engine/world/
  Entity.h            (EntityId + kEntityNone + componentTypeId<T>())
  ComponentArray.h    (templated dense + sparse storage)
  World.h / .cpp      (entity lifecycle + type-erased arrays)
  Transform.h         (POD Transform component)
engine/render/
  RenderHandles.h     (POD RenderHandles component)
games/11-sandbox/
  main.cpp            (loader + submit loop + Inspector→World sync + destroy)
```

No new external dependencies. The world module compiles into the existing
`ironcore` library target.

## Testing

`tests/test_world.cpp` (new):

**ComponentArray:**
- `add` then `get` returns a pointer to the stored value
- `remove` invalidates `get` (returns nullptr) and shrinks `size()` by 1
- After swap-and-pop remove, the swapped entity's `get` still returns the
  right value (sparse index correctly updated)
- Iteration order matches insertion order minus removed-via-swap
- Re-adding to a previously-removed entity reuses storage correctly
- `entityAt(row)` matches the entity that owns `dense_[row]`

**World:**
- `create()` returns a `valid()` `EntityId` with generation ≥ 1
- `destroy(e)` bumps the slot's generation; `alive(e)` returns false for
  the destroyed handle even after the slot is recycled
- Recycled slot returns an `EntityId` with a new generation; old handle
  stays invalid
- Multi-type add/get/remove on the same entity chains correctly
- `destroy(e)` tears off all components (each `view<T>()` no longer
  contains `e`)

**Render submit pseudocode:**
- Construct a World with three entities carrying Transform + MeshRef +
  MaterialDef + RenderHandles (dummy handles)
- Walk the Section 3 submit pseudocode; assert the produced `(Transform,
  MaterialDef, RenderHandles)` sequence matches expected

No Vulkan or sandbox needed. Pure-logic, fast. Target: 47 → ~60 tests.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| `sceneIndex → EntityId` table drifts out of sync after add/delete | The table mutates only inside the existing `scene.entities` add/delete code paths in the sandbox host. One commit touches both. |
| End-of-frame Inspector → World sync is the kind of thing that's easy to forget when adding new Inspector fields | Document the sync as the contract: "anything Inspector edits in `scene.entities[i]` must be mirrored to the matching World entity in the sync loop." Removed entirely in M39. |
| `RenderHandles` shape is renderer-specific (Vulkan today) and may not fit OpenGL | OpenGL is frozen ([[vulkan-only-direction]]); the component compiles fine against either backend's handle types because they're typedef'd at a common shim. |
| Template-heavy `componentTypeId<T>()` counter pattern can blow up compile times | Counter pattern is well-trodden (EnTT, Bevy reference impls); compile-time hit at 4 component types is negligible. |
| The dense vec + sparse index design forecloses on archetype storage if we ever need it | We don't, at TF2/Overwatch scale. If a future game needs it, `ComponentArray<T>` is a swap-out without changing the `World` API. |

## Success criteria

1. `iron::World` exists, owns three POD component types and `RenderHandles`,
   passes the unit test plan above.
2. `games/11-sandbox` loads `demo.json` into a `World`, renders by iterating
   the World, and survives add/delete/duplicate of entities exactly as it
   did before M37.
3. `47 → 60+` tests green; sandbox visually indistinguishable from M36.
4. Zero new external dependencies. No renderer changes.
5. The Inspector → World end-of-frame sync is the only special-case
   bridging logic; it's documented and confined to one place.

After M37: the foundation track moves to M38 (reflection / type registry),
which makes the Inspector + serialization data-driven and ends the sync
loop.
