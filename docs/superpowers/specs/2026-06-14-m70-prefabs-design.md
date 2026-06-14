# M70 Prefab Assets — Design Spec

**Date:** 2026-06-14
**Status:** Approved
**Author:** Elias Hanken (with Claude)
**Roadmap:** `docs/superpowers/2026-06-14-m70-m73-roadmap.md`

## Purpose

Let an authored subtree be saved as a reusable **prefab** asset and stamped back
into a scene any number of times. This is the first of the game-authoring
primitives (prefabs → spawn points → navigation → AI) that the wave-survival
arena needs, and that the eventual class-shooter needs for player/pickup
spawning.

**Stamp model (decided):** instantiating a prefab produces a *detached copy* —
the instance keeps no link to the `.prefab` file, and editing the file does not
change existing instances. Linked-with-overrides is explicitly out of scope (see
Non-Goals). This makes the whole feature a composition of M69's tested subtree
machinery plus SceneIO, with no new "live link" subsystem.

## Goals

- Save the selected subtree (a hierarchy root + all descendants) to a `.prefab`
  file that round-trips losslessly, including nested hierarchy and components.
- Instantiate a prefab into the current scene at a placement transform, with
  unique names, correct parent re-indexing, undo support, and World mirroring —
  reusing the existing add/duplicate code path.
- Keep the prefab math headless and unit-tested; keep the panel a pure
  intent-returning UI like `SceneOutliner`.
- One shared entity-JSON code path for scenes and prefabs (no schema drift).

## Non-Goals (YAGNI)

- No prefab→instance linking, no per-instance overrides, no "apply/revert to
  prefab." (Deferred; revisit only if a game needs it.)
- No nested-prefab-*reference* (a prefab containing another prefab is stored as
  flattened entities; there is no recursive prefab link).
- No drag-from-browser-into-viewport placement — instantiate goes to the
  standard `spawnPos()` (in front of the camera), matching every other add.
- No thumbnail rendering of prefabs; the browser is a text list for now.
- No automatic asset-dependency copying (mesh/texture paths are stored as
  strings and resolved at load, exactly as scenes do today).

## Data model

A prefab is a **self-contained subtree**: a `std::vector<SceneEntity>` where
index 0 is the root and every `parentIndex` is *prefab-local* (root = -1,
descendants index into the prefab's own vector). `SceneEntity` already carries
`Transform`, `MeshRef`, `MaterialDef`, and `ComponentSet`, so a prefab captures
geometry, materials, collision/audio/probe/logic components, and nested
hierarchy with no new per-field code.

```cpp
// engine/scene/Prefab.h
struct Prefab {
    int                      version = 1;
    std::vector<SceneEntity> entities;   // entities[0] is the root; parentIndex is prefab-local
};
```

## Architecture

Four units, each independently testable, plus host wiring.

### 1. Headless prefab core — `engine/scene/Prefab.{h,cpp}`

Pure free functions over `SceneFile` / `Prefab`, in the style of
`SceneHierarchy`. They reuse `collectSubtree` and follow `duplicateSubtree`'s
remap discipline (the high-defect-risk part, isolated and tested first).

```cpp
// Extract entity `root` and its whole subtree into a self-contained Prefab.
// Root becomes index 0; parentIndex values are re-based to prefab-local space
// (root = -1). The extracted root's transform is copied verbatim (placement is
// applied at instantiate time, not extract time). Returns an empty Prefab if
// root is out of range.
Prefab extractPrefab(const SceneFile& scene, int root);

// Append a prefab's entities to scene.entities. Internal parent links are
// re-indexed into scene space; names are uniquified via `uniquify`; the new
// root's transform is replaced by `placement` (its descendants keep their
// prefab-local transforms, so the subtree's internal layout is preserved).
// Returns the new root's index in scene.entities (-1 if the prefab is empty).
// Existing indices are unchanged (copies are appended), so the host only needs
// to spawn World entities for the appended range — identical to duplicateSubtree.
int instantiatePrefab(SceneFile& scene, const Prefab& prefab,
                      const Transform& placement,
                      const std::function<std::string(const std::string&)>& uniquify);
```

Design notes:
- `instantiatePrefab` re-indexing mirrors `duplicateSubtree` exactly: prefab
  index `k` maps to scene index `base + k`; root's `parentIndex` is set to -1
  (instantiated as a scene root); a descendant's `parentIndex` (prefab-local,
  always ≥ 0 and < prefab size) maps to `base + that`.
- Placement replaces only the **root** transform. Rationale: the prefab's own
  authored root transform is an authoring artifact (where it happened to sit when
  saved); callers want to place the instance where *they* choose. Descendants are
  parent-relative and must be preserved verbatim.

### 2. Shared entity-JSON helpers (refactor) — `engine/scene/EntityJson.h`

`entityToJson` / `entityFromJson` (and the small `toJson` / `readVec3` /
`readFloat` / `readString` helpers) currently live in the anonymous namespace of
`SceneIO.cpp`. Lift them into a new internal header `engine/scene/EntityJson.h`
(declarations) + keep definitions in a `EntityJson.cpp` (or inline), and have
`SceneIO.cpp` include it. This is a pure move — no behavior change — verified by
the existing `test_scene_io` continuing to pass. PrefabIO then reuses the *same*
entity schema, so prefab and scene entity fields can never drift apart.

### 3. Prefab serialization — `engine/scene/PrefabIO.{h,cpp}`

```cpp
bool savePrefabFile(const Reflection&, const ComponentRegistry&,
                    const Prefab&, const std::string& path);
std::optional<Prefab> loadPrefabFile(const Reflection&, const ComponentRegistry&,
                                     const std::string& path);
// String variants back the unit tests (no file I/O), mirroring SceneIO.
std::string prefabToJsonString(const Reflection&, const ComponentRegistry&, const Prefab&);
std::optional<Prefab> prefabFromJsonString(const Reflection&, const ComponentRegistry&,
                                           const std::string&);
```

File shape (pretty-printed on disk, like scenes):

```json
{ "version": 1, "entities": [ { "name": "...", "transform": {…}, "mesh": {…},
  "material": {…}, "components": [...] }, { "name": "...", "parent": 0, … } ] }
```

On load, run the **same parentIndex sanitize pass** SceneIO uses (out-of-range /
self-parent / cycle → reset to -1 with a warning) so a hand-edited prefab can't
corrupt a scene. Additionally: after sanitize, if `entities[0].parentIndex != -1`
it is forced to -1 (the root is the root by definition); a malformed/empty
`entities` array yields `nullopt` with a logged error.

### 4. Editor panel — `engine/editor/PrefabBrowser.{h,cpp}`

Pure UI returning an intent struct; the host performs all mutation — identical
contract to `SceneOutliner`.

```cpp
class PrefabBrowser {
public:
    struct Result {
        enum class Action { None, CreateFromSelection, Instantiate };
        Action      action = Action::None;
        std::string prefabPath;   // populated for Instantiate
    };
    // `selectionValid` enables the "Create Prefab from Selection" button.
    // `prefabPaths` is the host-supplied list of discovered .prefab files.
    Result draw(const std::vector<std::string>& prefabPaths, bool selectionValid);
private:
    char nameBuf_[128] = {};   // name for "Create Prefab from Selection"
};
```

Layout: a "Create Prefab from Selection" button + a name field (disabled when no
selection), a separator, then one selectable row per discovered prefab with an
"Instantiate" affordance. Directory discovery (globbing `assets/prefabs/*.prefab`)
is done host-side and passed in, keeping the panel pure and testable-by-eye.

### 5. Host wiring — `games/11-sandbox/main.cpp`

- **Discovery:** maintain a `std::vector<std::string> prefabPaths` refreshed
  (lazily / on create) from `assets/prefabs/`. Ensure the directory exists on
  first save.
- **Create:** on `CreateFromSelection` with a valid `selectedIndex`:
  `extractPrefab(scene, selectedIndex)` → `savePrefabFile(...,
  "assets/prefabs/<name>.prefab")` → refresh `prefabPaths`. No scene mutation, so
  no undo entry.
- **Instantiate:** on `Instantiate`: `loadPrefabFile(path)` →
  `instantiatePrefab(scene, *prefab, Transform{spawnPos()}, uniqueName)` → for
  the appended index range, resolve + create World entities (the exact loop the
  Duplicate branch already runs) → `mirrorParents()` → select the new root →
  set `structuralEdit = true` so it rides the existing whole-scene-JSON undo
  snapshot.

## Data flow

```
Create:  select root → PrefabBrowser(CreateFromSelection)
         → extractPrefab(scene, sel) → savePrefabFile → refresh list

Instantiate: PrefabBrowser(Instantiate, path) → loadPrefabFile
         → instantiatePrefab(scene, prefab, spawnPos)  [mutates scene.entities]
         → spawn World entities for appended range → mirrorParents
         → select new root → structuralEdit (undo snapshot)
```

## Error handling

- `extractPrefab` with an out-of-range root → empty `Prefab` (host treats empty
  as "nothing to save," logs, no file written).
- `loadPrefabFile`: missing file / malformed JSON / empty entities → `nullopt`
  + `Log::error`; host shows nothing changed. Out-of-range/cyclic parents →
  sanitized to roots with `Log::warn` (file still loads).
- `savePrefabFile`: unopenable path → `false` + `Log::error`.
- Name collision on save: the host appends a numeric suffix (reuse the
  `uniqueName` idea against existing files) so a save never silently clobbers.
- Instantiate where a child entity fails to resolve (e.g. bad glTF path): the
  failing entity is skipped exactly as the startup resolve loop does; the rest of
  the subtree still instantiates. (Inherited behavior; not new.)

## Testing strategy

Unit tests (CTest, `tests/test_framework.h`):

`tests/test_prefab.cpp` (headless core):
- `extractPrefab` on a 3-deep chain rebases parentIndex to prefab-local (root
  -1, mid 0, leaf 1) and copies components.
- `extractPrefab` → `instantiatePrefab` round-trips: appended count, new root
  index, re-indexed parent links, names uniquified, placement applied to root
  only (a descendant keeps its prefab-local offset).
- `instantiatePrefab` into a non-empty scene appends after existing entities and
  leaves existing indices untouched.
- Empty/out-of-range guards return -1 / empty without mutating the scene.

`tests/test_prefab_io.cpp` (serialization):
- `prefabToJsonString` → `prefabFromJsonString` round-trips a multi-entity
  hierarchy with a component (e.g. CollisionShape) intact.
- Sanitize: a prefab JSON with an out-of-range `parent` loads with that entity
  reset to root; root entity with a stray `parent` is forced to -1.

`tests/test_scene_io.cpp` (regression): unchanged and must still pass after the
EntityJson refactor (proves the move was behavior-preserving).

## Demo gate (user-run, milestone acceptance)

1. Build a 3-entity hierarchy in the sandbox; select its root.
2. "Create Prefab from Selection" with a name → `assets/prefabs/<name>.prefab`
   appears; the browser lists it.
3. Instantiate it 2–3 times → each copy appears in front of the camera as an
   independent subtree (move one; the others don't move — confirms stamp model).
4. Edit one instance, re-instantiate the prefab → the new copy reflects the
   *saved* prefab, not the edited instance (confirms no live link).
5. Ctrl+Z after an instantiate removes the whole instantiated subtree; redo
   restores it.
6. Save the scene, reload → instantiated subtrees persist (they are ordinary
   scene entities now).

## File summary

**Create:**
- `engine/scene/Prefab.h`, `engine/scene/Prefab.cpp`
- `engine/scene/EntityJson.h` (+ `.cpp` if definitions are moved out of line)
- `engine/scene/PrefabIO.h`, `engine/scene/PrefabIO.cpp`
- `engine/editor/PrefabBrowser.h`, `engine/editor/PrefabBrowser.cpp`
- `tests/test_prefab.cpp`, `tests/test_prefab_io.cpp`

**Modify:**
- `engine/scene/SceneIO.cpp` — include `EntityJson.h`; remove the moved statics.
- `engine/CMakeLists.txt` / `engine/editor/CMakeLists.txt` — add new sources.
- `tests/CMakeLists.txt` — register the two new tests.
- `games/11-sandbox/main.cpp` — PrefabBrowser panel + create/instantiate wiring.

## Open questions

None. Ready for implementation planning.
