# M34 — Object Placement (Design)

**Date:** 2026-05-30
**Milestone:** M34 (editor track — build a scene from scratch)
**Prerequisite:** M29 `SceneFile`/`saveSceneFile`; M30 `ironcore_editor` (`SceneOutliner`/`SceneInspector`); M31–M33 gizmo. The sandbox editor host resolves each `SceneEntity` into a `ResolvedEntity { int entityIndex; MeshHandle mesh; Material material; Mat4 model; Aabb localBounds; }`.

## Goal

Let the user **add, delete, and duplicate** entities in the editor: spawn a
cube/plane/glTF, delete the selected entity, duplicate it — then place it with
the gizmo and Save. This completes the editor's "build a scene from scratch"
loop; today you can only tune what's already in `demo.json`.

## Scope

**In scope:** add cube, add plane, add glTF (by path); delete the selected
entity; duplicate the selected entity. New/duplicated entity is auto-named,
spawned, and selected. Save persists via `saveSceneFile` (M29).

**Out of scope:** undo/redo, multi-select, drag-reorder in the Outliner, an
asset browser (glTF is added via a path field), grouping/parenting, freeing a
deleted glTF's GPU mesh (no `destroyMesh` API yet — a small, acknowledged leak).

## Architecture — Outliner emits intent, host executes

`SceneOutliner` (in `ironcore_editor`) sees only a `const SceneFile&` and must
not create GPU meshes or mutate/resolve the scene — that keeps the editor module
decoupled from the renderer. So the Outliner grows an **add bar** and returns the
user's *intent*; the sandbox **host** performs the mutation (it owns the
renderer, the `resolved` vector, and the selection).

```cpp
// engine/editor/SceneOutliner.h
struct OutlinerResult {
    bool saveClicked = false;
    enum class Action { None, AddCube, AddPlane, AddGltf, Delete, Duplicate };
    Action action = Action::None;
    std::string gltfPath;   // populated for AddGltf (from the path text field)
};

class SceneOutliner {
public:
    OutlinerResult draw(const SceneFile& scene, int& selectedIndex);
private:
    char gltfPathBuf_[256] = {};   // ImGui InputText buffer for the glTF path
};
```

The add bar (below the existing entity list + Save button):
`[+ Cube] [+ Plane]`, then `InputText(gltfPath)` + `[+ glTF]`, then
`[Duplicate] [Delete]` (Duplicate/Delete disabled/no-op when nothing is
selected). Each button sets `result.action`; `[+ glTF]` also copies the buffer to
`result.gltfPath`.

## Host: performing the actions (the resolved ↔ entities sync)

Factor the existing startup resolve-loop body into a helper the host reuses:

```cpp
// Resolve one SceneEntity to a ResolvedEntity (mesh handle + material + bounds).
// Returns false if a glTF entity fails to load.
bool resolveEntity(const SceneEntity& e, int entityIndex, ResolvedEntity& out);
```

(It uses the same primitive-mesh cache + glTF load + texture/material resolution
the startup loop already does; the startup loop becomes a call to it per entity.)

Actions:
- **AddCube / AddPlane / AddGltf:** build a new `SceneEntity` (defaults below),
  `push_back` onto `scene.entities` at index `N = entities.size()-1`,
  `resolveEntity(newEntity, N, re)`; on success push `re` onto `resolved` and set
  `selectedIndex = N`. For AddGltf, if `resolveEntity` fails (bad path), log a
  warning and roll back the `push_back` (don't add).
- **Duplicate:** copy `scene.entities[selectedIndex]`, offset its position
  slightly (e.g. +0.5 on X) and give it a fresh unique name, then add it exactly
  like Add (append + resolve + select).
- **Delete `d`** (the selected index): `erase(entities.begin()+d)`; erase the
  `resolved` entry whose `entityIndex == d` (if present); then **decrement
  `entityIndex` on every remaining resolved entry with `entityIndex > d`** so the
  mapping stays correct (append-only adds never need this; delete does). Set
  `selectedIndex = -1`.

This keeps `resolved[]` aligned to `scene.entities` without rebuilding all GPU
meshes (only a newly-added entity resolves a mesh; primitive meshes are shared
via the existing cube/plane cache). A deleted glTF entity's GPU mesh handle is
not freed (no `destroyMesh`) — an acknowledged minor leak.

## New-entity defaults

- **name:** auto-unique. Base (`"cube"`, `"plane"`, or the glTF file stem) plus
  the smallest integer suffix that isn't already used (`"cube"`, `"cube 2"`, …).
- **transform:** `position = cam.position + cam.forward() * 5.0f` (in front of the
  camera, immediately visible); `rotation = identity`; `scale = {1,1,1}`.
- **mesh:** `MeshRef{ primitive = Cube|Plane }` or `MeshRef{ gltfPath = <path> }`.
- **material:** default (`MaterialDef{}` — empty texture paths resolve to the
  engine's white / flat-normal / no-spec fallbacks, so the entity is visible;
  emissive 0, uvScale 1, reflectivity 0).
- Duplicate copies the source entity's transform/material verbatim, then offsets
  position and renames.

## Input

- Outliner buttons drive all three actions.
- **Delete** key → delete the selection; **Ctrl+D** → duplicate — only when not
  flying (RMB up) and `!imgui.wantsKeyboard()` (so typing in the path field or
  inspector doesn't trigger them).

## Flow

Add → entity appears ~5 units in front of the camera, auto-selected with its
gizmo → drag the gizmo to place → **Save Scene** writes it to `demo.json`.
Relaunch loads the augmented scene. Build levels in the viewport, not in JSON.

## Testing & verification

- **Automated:** none new — the add/delete/reindex logic is host-side (sandbox)
  and the add-bar is editor-UI; neither is reachable by the `ironcore`-linked test
  harness. The existing suite (46) must stay green.
- **Visual (user-verified, the gate):** add cube/plane/glTF → appears in front of
  the camera, selected, gizmo on it; bad glTF path → warning, nothing added;
  delete → entity gone, selection cleared, other entities still correct (gizmo
  picks the right ones); duplicate → offset copy, selected; Save → relaunch shows
  the new scene.

## Known v1 limitations

- No undo/redo, multi-select, parenting, or asset browser (glTF via path field).
- Deleting a glTF entity leaks its GPU mesh (no `destroyMesh` yet).
- glTF paths resolve relative to the executable directory (same as M29).

## File structure

**Modify:**
- `engine/editor/SceneOutliner.h` / `.cpp` — `OutlinerResult` + the add bar.
- `games/11-sandbox/main.cpp` — `resolveEntity` helper (refactor the startup loop
  onto it), action handling (add/delete/duplicate + reindex), spawn-in-front +
  unique naming, Delete/Ctrl+D shortcuts.
- `docs/engine/editor.md` — document add/delete/duplicate.
