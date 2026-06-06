# M57 — Editor Undo/Redo — Design

> **Editor-infrastructure milestone.** Adds Ctrl+Z / Ctrl+Y "smart history" to the editor, across **both** the scene (entity edits) and the node graph. Driven by a direct user ask after M56: "we have no CTRL+Z history as well for anything … would be really nice to have," then expanded — "lets have an undo stack on all interfaces like scene and nodes etc … can easily implement the stack on new features also." So this milestone delivers a **reusable, document-agnostic undo stack** that scene + graph plug into, and that any future authoring surface (shader graph, VFX, UI nodes, environment settings) can adopt by supplying a serialize/restore pair.

## Goal

Press **Ctrl+Z** to revert the last edit and **Ctrl+Y** / **Ctrl+Shift+Z** to re-apply it, in whichever panel has focus. A single user gesture = a single undo step (a gizmo drag, a slider drag, an add, a delete, a node move — one entry each, not one-per-frame). The mechanism is a generic snapshot/memento stack reused by both documents and trivially extensible to new ones.

## Scope (locked in brainstorming)

**In:**
- Scene document: add / delete / duplicate entity, Inspector field edits, gizmo transforms, component add/remove — anything that mutates `scene.entities`.
- Node-graph document: add / delete node, connect / disconnect, literal edits, node-position drags.
- A generic `UndoHistory` (snapshot stack) instantiated once per document.
- Per-panel routing by focus; coalescing so one gesture = one entry; capacity cap.
- Lifecycle: **cleared on entering Play**, **survives Save**.

**Out (explicitly deferred):**
- Environment / render-setting undo (bloom, fog, exposure, skybox swap).
- A visual history panel / named steps.
- Undo across Save/Load of a *different* scene file.
- Redo branching / multi-level trees.
- Cross-document merged "global" undo (each panel undoes its own document; this is the chosen routing, not a limitation to fix later).

## Architecture

The editor already has a clean invariant that makes **snapshot/memento** the right model: `scene.entities` is the **single source of truth**, and `World` (ECS), `resolved` (render data), and `sceneIndexToEntity` (parallel map) are **derived mirrors** rebuilt from it. Likewise the node graph's source of truth is `GraphEditorModel`, which already round-trips through JSON. So an undo step is just: *snapshot the source-of-truth document to a string before an edit; on undo, restore the string and rebuild the derived state.* This sidesteps the desync bugs a command/diff model would invite (every mirror is rebuilt wholesale, never patched).

**Generic core, two instances.** One class, `UndoHistory`, stores opaque `std::string` snapshots and knows nothing about scenes or graphs. The host owns two instances — `sceneHistory_` and `graphHistory_` — and supplies the document-specific serialize/restore:

- **Scene** serialize = `sceneToJsonString(reflection, scene)`; restore = `sceneFromJsonString(...)` then `rebuildDerivedFromScene()` (World + resolved + sceneIndexToEntity + clamp `selectedIndex`).
- **Graph** serialize = `graphModel.toJson().dump()`; restore = `graphModel.loadFromJson(parsed)` then `nodeGraphPanel.resetPlacement()` (so saved node positions re-apply, reusing the M56 path).

Because the contract is just "string in / string out + a rebuild hook," **future surfaces adopt undo by adding one more `UndoHistory` + a serialize/restore pair** — no changes to the core. This is the "easily implement the stack on new features" the user asked for.

### `UndoHistory` (engine/editor/UndoHistory.{h,cpp})

```cpp
class UndoHistory {
public:
    explicit UndoHistory(std::size_t capacity = 100);

    // Push a pre-edit snapshot as a new undo entry; clears redo; evicts oldest
    // beyond capacity.
    void commit(std::string beforeSnapshot);

    bool canUndo() const;
    bool canRedo() const;

    // Given the document's CURRENT serialized state, return the snapshot to
    // restore to (and bookkeep the opposite stack). nullopt = nothing to do.
    std::optional<std::string> undo(const std::string& current);
    std::optional<std::string> redo(const std::string& current);

    void clear();

private:
    std::vector<std::string> undo_;
    std::vector<std::string> redo_;
    std::size_t capacity_;
};
```

Semantics:
- `commit(before)` → `undo_.push_back(before)`; if `undo_.size() > capacity_` drop `undo_.front()`; `redo_.clear()`.
- `undo(current)` → if `undo_` empty return `nullopt`; else `redo_.push_back(current)`; pop+return `undo_.back()`.
- `redo(current)` → if `redo_` empty return `nullopt`; else `undo_.push_back(current)`; pop+return `redo_.back()`.
- `clear()` empties both.

Note the snapshot semantics: we store the state **before** each edit on the undo stack, and the **current** state is captured at the moment undo/redo is requested. This keeps "where we are now" out of the stack until the user actually time-travels, which is what makes redo correct without a sentinel "present" entry.

### Coalescing: one gesture = one entry

A naive "snapshot every frame the document differs" yields hundreds of entries per gizmo drag. The unified rule (run per document, every frame, after edits are applied):

1. `cur = serialize(document)` — but only when an edit is *plausible* this frame (see gating), else skip entirely (idle cost = zero).
2. If `cur != last`: a change is in flight. If no transaction is open, **open one** (`pendingBefore = last`). Set `last = cur`.
3. **Close + commit** the open transaction only when the document is *stable and the user isn't mid-interaction*: `!ImGui::IsAnyMouseDown() && !gizmo.dragging()`. On close: `history.commit(pendingBefore)`; clear the pending flag.

This collapses a whole drag (many frames of change while the mouse is down) into a single `commit` of the pre-drag snapshot, and also handles discrete edits (add/delete/connect — change appears one frame, mouse already up next frame → commit). The same rule serves both documents; the graph uses the node-editor's own drag state implicitly via `IsAnyMouseDown` (node drags hold the mouse).

**Gating (keep idle cost zero).** Only serialize when an edit is plausible this frame: Inspector returned changed, gizmo is active or just released, an add/delete/duplicate fired, or the Node Editor is focused / the graph model reports `dirty()`. Absent any signal, skip the serialize. The graph already exposes `dirty()`/`clearDirty()`; the scene side keys off the existing edit-event booleans the host already computes.

### Applying a restore must not re-record

After `undo`/`redo` writes a restored snapshot back into a document, set that document's `last` to the restored string and clear any open transaction, so the next-frame compare sees no delta and nothing re-commits.

## Routing & lifecycle

- **Shortcuts:** Ctrl+Z = undo, Ctrl+Y and Ctrl+Shift+Z = redo. Suppressed while a text field is active (`imgui.wantsKeyboard()`) so in-field text editing keeps its own behavior.
- **Routing by focus:** if the **Node Editor** window is focused → graph history; otherwise → scene history. Reuses the focus check added in M56 for delete-gating (`viewport.focused` and the Node Editor window's `IsWindowFocused`).
- **Cleared on Play:** entering Play calls `clear()` on both stacks (Play is transient; M55 already snapshots/restores the scene around Play/Stop, so undo history there would be meaningless).
- **Survives Save:** Save does not touch history.
- **Capacity:** default 100 entries per stack; oldest evicts. (Snapshots are small JSON strings; 100 is generous and bounded.)

## In-memory serialization helpers

`SceneIO.h` today only exposes **file-based** `loadSceneFile` / `saveSceneFile`; the entity↔JSON logic (`entityToJson`/`entityFromJson`) is internal to `SceneIO.cpp`. M57 adds thin in-memory string helpers, factored from the existing file path so there's exactly one serializer:

```cpp
// SceneIO.h
std::string sceneToJsonString(const Reflection& reflection, const SceneFile& scene);
std::optional<SceneFile> sceneFromJsonString(const Reflection& reflection,
                                             const std::string& json);
```

`saveSceneFile` becomes `sceneToJsonString` + write-file; `loadSceneFile` becomes read-file + `sceneFromJsonString`. The graph already has `toJson()`/`loadFromJson()` so no new graph IO is needed.

## Error handling

Snapshots come from our own serializers and always round-trip. `sceneFromJsonString` / `loadFromJson` already fail safe (log + no-op) on malformed input, so a bad snapshot can never crash a restore. Undo/redo on an empty stack is a no-op (`nullopt`). A restored `selectedIndex` that's now out of range clamps to "deselected" inside `rebuildDerivedFromScene()`.

## Testing

- **New `tests/test_undo_history.cpp`** — exercises the generic stack headlessly with plain strings (no engine deps):
  - commit → undo returns the committed snapshot; redo returns the captured "current."
  - redo stack cleared by a fresh `commit`.
  - capacity eviction (commit > cap, oldest dropped, `canUndo` stays true, deepest entry gone).
  - empty-stack `undo`/`redo` → `nullopt`.
  - round-trip invariant: undo(cur) then redo(restored) returns `cur`.
- Scene/graph JSON round-trips are already covered by existing SceneIO + `test_graph_editor` tests; `sceneToJsonString`/`sceneFromJsonString` are exercised transitively (the file helpers now call them) plus a direct round-trip assertion.
- The transaction wiring, coalescing (one-gesture-one-entry), restore-rebuild, routing, and clear-on-Play are **visual-gated**: undo/redo a gizmo move, an add, a delete, a duplicate, an Inspector edit; then in the Node Editor undo/redo a node move, a node add/delete, and a connect/disconnect; confirm focus routes each to the right document and a single drag is one step.

## Files

**New:**
- `engine/editor/UndoHistory.h` / `engine/editor/UndoHistory.cpp` — the generic stack.
- `tests/test_undo_history.cpp` — headless unit tests.

**Modified:**
- `engine/scene/SceneIO.h` / `SceneIO.cpp` — add `sceneToJsonString` / `sceneFromJsonString`; refactor file helpers to use them.
- `games/11-sandbox/main.cpp` — two `UndoHistory` members; factor the appendAndSelect/load mirror-rebuild into `rebuildDerivedFromScene()`; per-frame coalescing transaction loop per document; Ctrl+Z/Y routing by focus; `clear()` on entering Play.
- `engine/CMakeLists.txt` — add `UndoHistory.cpp` to the editor lib.
- `tests/CMakeLists.txt` — register `test_undo_history`.

## Notes for the plan

- `rebuildDerivedFromScene()` is the linchpin — it must exactly reproduce what add/delete/duplicate/load already do today (World rebuild, resolved rebuild, sceneIndexToEntity, selection clamp). Factor it from the existing `appendAndSelect` + load paths first, switch those call sites onto it, and confirm no behavior change **before** wiring undo on top.
- Keep `UndoHistory` engine-side and ImGui-free so the unit test links without a window/context.
- The coalescing "document stable" check (`!IsAnyMouseDown() && !gizmo.dragging()`) is the one subtle bit — verify in the visual gate that a slow gizmo drag produces exactly one undo entry, not zero and not many.
