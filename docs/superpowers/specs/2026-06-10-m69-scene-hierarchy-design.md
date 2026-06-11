# M69: Scene Hierarchy — Design

**Date:** 2026-06-10
**Status:** Approved (design dialogue 2026-06-10; user approved "looks right")
**Approach:** A+ — `parentIndex` on `SceneEntity` (order-free) + a `Parent` World component mirror, runtime composes through the World.

## Goal

Parent/child entity transforms with reparenting in the outliner. When a parent
moves, its children follow — in Edit mode **and** during Play (physics- or
graph-driven parents drag their subtree). Biggest missing scene-authoring
primitive; prerequisite for prefabs (M72).

## Decisions (locked with user)

1. **Edit + Play:** hierarchy is real at runtime, not baked at Play start.
2. **Delete removes the whole subtree** (Unreal/Unity behavior; whole-scene
   JSON undo makes it recoverable).
3. **Reparent keeps world position:** local is recomputed as
   `inverse(parentWorld) × childWorld` (Unreal/Unity default).
4. **Inspector shows local transform** (relative to parent); root entities
   unchanged.
5. **Storage = A+ hybrid** (not bones-style DFS ordering, not a full ECS
   source-of-truth flip): reparenting writes one int, all index-parallel
   arrays (`selectedIndex`, `sceneIndexToEntity`, `resolved`) stay valid, old
   scene files load unchanged. The full "World as source of truth + stable
   entity IDs" migration is its own roadmap milestone (~M70.5, before M71
   entity refs / M72 prefabs); M69's `Parent` mirror means only the
   editor-side flip will remain then.

## Design

### 1. Data model

- `SceneEntity` gains `int parentIndex = -1` (`engine/scene/SceneFormat.h`).
  -1 = root; otherwise an index into `scene.entities`.
- **SceneIO:** `entityToJson` writes `"parent": parentIndex` only when ≠ -1;
  `entityFromJson` defaults to -1 — every existing scene file loads
  unchanged. Load-time validation: out-of-range or cyclic `parentIndex` is
  reset to -1 with `Log::warn` (fail-safe, matching SceneIO's posture).
- New World POD component `Parent { EntityId parent; }`
  (`engine/world/Parent.h`). Engine-internal like `RenderHandles`: NOT in the
  ComponentRegistry, never appears in Inspector/Add Component. Written by
  `spawnRuntime` / `rebuildDerivedFromScene` from `parentIndex` via
  `sceneIndexToEntity`.

### 2. Hierarchy math — `engine/scene/SceneHierarchy.{h,cpp}` (headless, tested)

Free functions over `SceneFile`:

- `worldMatrixOf(scene, i) -> Mat4` — walk up the parent chain
  (`local = translation × rotation.toMat4() × scaling`), depth-capped
  (e.g. 256) as a cycle guard.
- `isDescendant(scene, ancestor, maybeDescendant) -> bool`.
- `collectSubtree(scene, root) -> std::vector<int>` (root first).
- `reparentKeepWorld(scene, child, newParent) -> bool` — rejects self /
  descendant / out-of-range; recomputes local via
  `decomposeTRS(inverse(parentWorld) × childWorld)`. **Documented lossy**
  under non-uniform parent scale combined with child rotation (shear has no
  TRS representation) — the standard engine caveat.
- `deleteSubtree(scene, root) -> std::vector<int>` and
  `duplicateSubtree(scene, root) -> ...` — mutate `scene.entities`, fix up
  every remaining `parentIndex`, and return an old→new index mapping
  (`-1` = removed) so the host can remap `sceneIndexToEntity`, `resolved`,
  and `selectedIndex` in one place. This generalizes the remap dance the
  existing single-delete path already does (main.cpp ~1596) — the fiddliest
  part of the milestone, mitigated by being a pure tested function.
  Duplicated subtrees keep internal parent links, attach to the source's
  parent, and get `uniqueName`d.
- `Transform::matrix()` helper on `engine/world/Transform.h` — factors the
  `translation × rotation × scale` composition currently inlined at render
  submit (main.cpp ~1759), picking (~1330), and gizmo paths.

### 3. Render / picking / physics — one composition path for both modes

- Render-submit and picking compose model matrices through the **World-side**
  `Parent` chain with a per-frame memo (indexed by scene index; no recursion
  blowup). In Edit mode the per-frame scene→World mirror makes this identical
  to scene-side composition; in Play, a parent moved by a logic graph or a
  dynamic physics body drags its subtree automatically. This single code path
  is what satisfies the Edit+Play decision with no extra runtime machinery.
- **Physics:** bodies spawn (Edit→Play) from the *composed* world pose.
  Dynamic-body write-back (world pose from Jolt) inverse-composes through the
  parent's current world matrix before storing local. Scale handling
  unchanged from M42 (physics ignores scale).
- **Logic-graph nodes:** `GetPosition`/`SetPosition`/`Translate` stay
  LOCAL-space; documented in `GameplayNodes`. `GetWorldPosition` /
  `SetWorldPosition` variants deferred to M70/M71. (M68's health-drain demo
  cube is a root — unaffected.)

### 4. Outliner (`engine/editor/SceneOutliner`)

- Tree view replaces the flat list: build a children adjacency per frame from
  `parentIndex`; recursive `ImGui::TreeNodeEx` (OpenOnArrow; Leaf flag when
  childless; Selected flag preserves current behavior; DefaultOpen).
- **Drag-drop reparenting:** every row is a drag source (payload = entity
  index) and a drop target; a drop zone below the tree (or on the window
  background) unparents (newParent = -1). Illegal drops (self, own
  descendant) render no target / no-op. Emits new
  `Result::Action::Reparent` with `{childIndex, newParentIndex}`; the host
  calls `reparentKeepWorld` + pushes undo.
- Delete → subtree; Duplicate → deep subtree copy under the same parent;
  Add (cube/plane/glTF) unchanged — new entities are roots.

### 5. Gizmo (world-space proxy)

For a child entity the gizmo manipulates a world-space proxy transform:
`proxy = decomposeTRS(parentWorld × localMatrix)`; the gizmo edits the proxy
exactly as it edits a root entity today; write-back is
`local = decomposeTRS(inverse(parentWorld) × proxyMatrix)`. Root entities take
the existing path untouched (parentWorld = identity short-circuit). Same
lossy-decompose caveat as reparenting; same as Unity/Unreal.

### 6. Untouched by design (verify, don't modify)

- **Undo/redo:** whole-scene JSON snapshots already carry `parentIndex`;
  `rebuildDerivedFromScene` gains only the `Parent`-component rebuild.
- **Play/Stop:** `editScene`/`editWorld` snapshots are value-semantic;
  `Parent` rides the existing `World` deep-copy (`IComponentArray::clone`).
- **Inspector:** transform fields remain the local values (decision 4).

### 7. Tests (`tests/test_scene_hierarchy.cpp` + extensions)

- 3-deep chain world-matrix composition vs hand-computed expectation
  (translate + rotate + scale at each level).
- `reparentKeepWorld`: child's world matrix preserved within epsilon across
  reparent; rejection cases (self, descendant, out-of-range).
- Cycle guard: hand-corrupted `parentIndex` cycle doesn't hang/crash
  `worldMatrixOf`.
- `deleteSubtree` / `duplicateSubtree`: returned index mapping correct;
  surviving `parentIndex` links correct; duplicate preserves internal
  structure and attaches to the source's parent.
- SceneIO: `parentIndex` round-trip; legacy scene without `"parent"` loads
  with -1; out-of-range parent sanitized with warn.
- `Transform::matrix()` equals the previous inline composition.

### 8. Demo / visual gate (user-run, sandbox)

1. Drag a cube onto cube-red in the outliner → tree nesting appears; child
   keeps its world position.
2. Rotate/move cube-red with the gizmo → child orbits/follows.
3. Inspector on the child shows local (parent-relative) values.
4. Play (F5): M68 health-drain sinks cube-red → child sinks with it.
   Stop restores both.
5. Delete cube-red → child goes too; Ctrl+Z restores both, hierarchy intact.
6. Duplicate cube-red → copies the subtree.

## Risks / accepted limitations

- TRS decompose is lossy under non-uniform parent scale + child rotation
  (shear). Accepted + documented; standard engine behavior.
- Subtree delete/duplicate index remapping is the highest-defect-risk code;
  isolated as pure functions returning the mapping, tested first (TDD).
- Index-based entity identity remains (refs break on delete outside the
  remapped arrays) — explicitly deferred to the World-migration milestone
  (~M70.5) where stable EntityIds become the forcing function.
