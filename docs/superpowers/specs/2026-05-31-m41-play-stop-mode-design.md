# M41 — Play/Stop Mode Infrastructure

**Status:** approved 2026-05-31
**Predecessor:** M40 — view-nav widget + camera affordances (PR #61, `43ec219`)
**Successor (planned):** M42 — CollisionShape component authoring

## Goal

Introduce **Edit Mode** and **Play Mode** to the sandbox editor — the foundational separation every engine that runs runtime simulation needs. In Edit mode, the user authors the scene (everything static, fully editable). In Play mode, runtime systems simulate: physics steps each frame, audio listener updates, and (in future milestones) components like `CollisionShape` materialize into Jolt bodies, `AudioEmitter` starts playing, scripts run.

This milestone is **infrastructure** — the toggle, the snapshot/restore plumbing, the UI, and a tiny demonstration. No new components ship. M42+ consumes it.

## Why now

The original M41 plan was CollisionShape + AudioEmitter component authoring. The user raised the right architectural objection during brainstorm: if collision and audio "just run all the time" the moment a scene loads, the editor is unusable — cubes fall while you try to position them, ambient sound plays while you click panels. **Every mainstream engine separates Edit/Play.** Adding Play/Stop infrastructure first gives M42-M44 (collision, audio, picking-through-floor) a clean substrate to land into.

## Non-goals (explicit deferrals)

- **Pause state** — only Play and Stop in v1. Pause is genuinely useful for debugging gameplay but its snapshot/restore semantics are subtle ("if I edit during Pause and then Resume, what happens?"). Defer to M41.5 or later when actual gameplay needs it.
- **Game camera switching** — Play mode keeps the free-fly camera as a spectator view. Switching to a "player camera" requires a `PlayerCamera` component concept we don't have yet.
- **Edit during Play** — Inspector edits, gizmo drag, add/delete are disabled in Play mode. Avoids confusing Jolt body state by mutating positions mid-simulation. M41.5 could add "edit during play with discard-on-stop" semantics if useful.
- **Disk-backed snapshot** — always in-memory. If the sandbox crashes during Play, no state is preserved (matches Unity behavior).
- **Multi-snapshot / undo-style history** — single backup slot, overwritten each Play.
- **CollisionShape, AudioEmitter, scripting integration** — those are M42, M43, M46+. M41 only provides the substrate.

## Architecture

### New module: `engine/editor/EditorState.{h,cpp}`

Lives in `ironcore_editor`. Deliberately thin — holds the mode flag and nothing else. Snapshot/restore stays in the sandbox because it owns the state being snapshotted (`SceneFile`, `World`, `FreeFlyCamera`).

```cpp
// engine/editor/EditorState.h
#pragma once

namespace iron {

class EditorState {
public:
    enum class Mode { Edit, Play };

    Mode mode() const { return mode_; }
    bool isPlaying() const { return mode_ == Mode::Play; }

    // Transition. EditorState only holds the mode flag — the caller
    // (sandbox) is responsible for snapshot/restore around mode changes.
    void setMode(Mode m) { mode_ = m; }

private:
    Mode mode_ = Mode::Edit;
};

}  // namespace iron
```

### What changes between modes

| State | Edit | Play |
|---|---|---|
| `physics.step(dt)` | not called | called each frame with `t.deltaSeconds` |
| `audio.setListener(...)` | called each frame (cheap, no harm) | called each frame |
| Editor panels (Inspector / Outliner / Environment / ViewGizmo) | visible | visible |
| Visual indicator | none | colored border + "▶ PLAY" text overlay |
| Scene mutation (Inspector edits / entity gizmo drag / add / delete) | enabled | disabled (sandbox skips the editing block while in Play; no spurious mutations) |
| Camera affordances (WASD + RMB-look + view-gizmo + Iso button + wheel zoom + MMB orbit) | enabled | enabled (all camera control stays available — spectating is the whole point of Play mode's persistent free-fly) |

**Editor-input gating in Play mode:** sandbox checks `editor.isPlaying()` and skips the mutating editor calls (`inspector.draw` → still draws, but the host doesn't write the mutations back; `gizmo.update` → not called; add/delete shortcuts → not called). The View-gizmo widget and Iso button stay functional because they only move the camera (which is allowed in Play). The simplest gate: skip the entire "scene editing" block in `setUpdate` while `editor.isPlaying()`.

### Snapshot/restore

Stored in the sandbox `main()` as three locals adjacent to the live state:

```cpp
iron::SceneFile     scene;
iron::World         world;
iron::FreeFlyCamera cam;

// Snapshot slots (zero-init; only valid while in Play mode).
iron::SceneFile     editScene;
iron::World         editWorld;
iron::FreeFlyCamera editCam;
```

**On Edit → Play:**
1. Copy `scene` → `editScene` (`SceneFile` is value-copyable today)
2. Copy `world` → `editWorld` (`World` needs to be value-copyable — verify; if not, add a `World::clone()` helper)
3. Copy `cam` → `editCam` (`FreeFlyCamera` is trivially copyable)
4. Spawn debug falling cube (see "Demonstration" below)
5. `editor.setMode(Play)`

**On Play → Edit:**
1. `scene = editScene;`
2. `world = editWorld;`
3. `cam   = editCam;`
4. Despawn debug falling cube (destroy its Jolt body, clear its bookkeeping)
5. `editor.setMode(Edit)`

If `World` is not value-copyable (depends on M37 implementation), add a `World::clone() → World` helper or `void copyTo(World&)`. The fix is small and engine-side.

### UI — Play toolbar

A new tiny ImGui window at the top-center of the viewport. Two states:

**Edit:**
```
[ ▶ Play ]
```

**Play:**
```
[ ■ Stop ]   PLAY MODE
```

The ` PLAY MODE` text is also drawn over the entire viewport as a colored border (e.g. orange 4-pixel border around the window's render area, drawn via ImGui's foreground draw list). Mirrors the affordance Unity uses (its play-tinted Game/Scene view).

### Triggers

Three ways to toggle:
- **F5** — hotkey, polled via `input.keyPressed(GLFW_KEY_F5)` in the sandbox loop
- **Play/Stop button** in the toolbar (LMB on the button)
- **Esc** (only while in Play mode) — exits to Edit. Currently `Esc` clears the selection in Edit mode; this stays, but in Play mode `Esc` first deselects (if anything selected) and exits to Edit otherwise. Actually simpler: `Esc` in Play mode always exits Play (selection deselect is unimportant during Play). Document this in the inline code comment.

### Physics integration

The sandbox doesn't currently construct a `PhysicsWorld`. M41 adds it:

```cpp
iron::PhysicsWorld physics;
if (!physics.init()) {
    iron::Log::error("sandbox: physics init failed");
    return 1;
}
```

Per-frame in `setUpdate`:
```cpp
if (editor.isPlaying()) {
    physics.step(t.deltaSeconds);
}
```

`physics.step(dt)` is deterministic and cheap when there are no bodies (M41's only body is the demo falling cube). M42 + future milestones populate it from `CollisionShape` components.

### Audio listener

The sandbox doesn't currently call `audio.setListener(...)`. M41 adds it (called every frame in both modes; cheap and always-correct):

```cpp
audio.setListener(cam.position, cam.forward(), iron::Vec3{0.0f, 1.0f, 0.0f});
```

Audio playback isn't gated to Play mode — the `AudioEngine` only plays when something *calls* `playSoundAt`, which nothing does yet in the sandbox. M43 (`AudioEmitter`) will gate emitter playback on `editor.isPlaying()` when it lands.

## Demonstration — the "debug falling cube"

So M41 has something visible to validate (no real components materialize yet), entering Play spawns a tiny demo body:

- **What:** A 1×1×1 dynamic cube at world position `(0, 5, 0)` with mass 5 kg, no rotation, default Jolt material
- **Floor:** Spawn a 20×0.5×20 static box at `(0, -0.25, 0)` as a floor the cube can land on (so it doesn't fall forever)
- **When:** On Edit → Play transition (after snapshot)
- **Removed:** On Play → Edit transition (before restore — destroy both bodies via `physics.destroyBody`)
- **Render:** the cube is rendered as a debug AABB via the existing `GizmoRegistry::aabb` API (the M11 in-viewport debug-shape system). Color = bright magenta so it's clearly a "test" object, not scene content.
- **State:** Tracked via two `iron::BodyId` locals in `main()` (`debugCube`, `debugFloor`), both `kInvalidBody` while in Edit mode

This demonstration proves: physics integrates only in Play, snapshot/restore doesn't touch the user's scene (`scene.entities` unchanged), Play/Stop toggling works end-to-end.

## File map

**New:**
- `engine/editor/EditorState.h`
- `engine/editor/EditorState.cpp` (small or empty — class is mostly inline)
- `tests/test_editor_state.cpp`

**Modified:**
- `engine/editor/CMakeLists.txt` — add `EditorState.cpp` to `ironcore_editor`
- `tests/CMakeLists.txt` — `iron_add_test(test_editor_state test_editor_state.cpp)`
- `games/11-sandbox/main.cpp` — construct `EditorState`, `PhysicsWorld`; snapshot/restore plumbing; F5 + Esc + toolbar handling; per-frame `physics.step` gate; `audio.setListener` per frame; debug cube spawn/despawn on transitions; editor-edit gating

**Potentially modified (verify during implementation):**
- `engine/world/World.h` — add `clone()` or `copyTo()` if value copy isn't already supported

**Untouched on purpose:** Renderer, ReflectionInspector, SceneIO, ViewGizmo, Gizmo, picking, all shipping games, all reflection sidecars.

## Tests

`tests/test_editor_state.cpp` (~3 named subtests):
- `test_editor_state_starts_in_edit_mode` — `EditorState().mode() == Edit`
- `test_set_mode_play_flips_state` — `setMode(Play)` makes `isPlaying()` true
- `test_set_mode_round_trip` — Edit → Play → Edit returns isPlaying() to false

Sandbox snapshot/restore correctness is visual-gated. The pure-logic substrate (the mode flag) is the only thing unit-testable; the rest is integration.

51 → 52 CTest entries.

## Phases (for the plan)

- **A — EditorState module + tests.** Create `engine/editor/EditorState.{h,cpp}` and `tests/test_editor_state.cpp`. Pure logic, TDD. 1 task.
- **B — Sandbox plumbing.** Add `PhysicsWorld` + `AudioEngine` listener calls. F5 / Esc / toolbar UI. Snapshot/restore. Editor-edit gating during Play. 1 task.
- **C — Debug falling cube demonstration.** Spawn floor + cube on Edit→Play, destroy on Play→Edit. Debug-render via `GizmoRegistry::aabb`. 1 task.
- **D — Visual gate + PR + merge + memory.** 1 task.

4 tasks total. M40-sized.

## Acceptance criteria

1. `EditorState::mode()` defaults to `Edit`; `setMode(Play)` flips to Play; `isPlaying()` correct in both states.
2. Sandbox toolbar shows "▶ Play" in Edit mode and "■ Stop" + "PLAY MODE" label + colored border in Play mode.
3. F5 toggles between modes from either state.
4. Esc in Play mode exits to Edit.
5. While in Play mode, the debug cube falls under gravity and lands on the debug floor.
6. Toggling Play → Stop → Play in the same session reuses the same snapshot mechanism cleanly (no stale state).
7. Scene entities (floor / cubes / helmet from `demo.json`) are NOT affected by Play mode — their positions/rotations/scales are exactly the same after Play→Stop as they were before Play.
8. Camera position / yaw / pitch are exactly restored on Play→Stop (user returns to their Edit-mode pose).
9. Inspector edits, gizmo drags, and add/delete are silently disabled in Play mode (no crashes, no spurious mutations).
10. WASD + RMB-look + view-gizmo + MMB orbit + wheel zoom continue to work in Play mode (camera affordances are not gated).
11. 51 → 52 tests green.
12. Renderer / shipping games / reflection layer untouched.

## Out of scope (deferred, for clarity)

- Pause state (M41.5+)
- Game-camera switching (post-"Player" concept)
- Edit-during-Play with discard-on-stop
- Persistent disk snapshot
- Multi-snapshot history / undo
- CollisionShape, AudioEmitter, scripting, AI, animation triggers — all M42+

## Open questions resolved during brainstorm

- *Two states or three (Pause)?* — Two. Pause is genuinely useful but adds snapshot/restore complexity that's better tackled after we have gameplay to pause.
- *Camera behavior in Play?* — Free-fly persists as spectator. Game camera is its own concept and waits for "Player" semantics.
- *Snapshot scope?* — SceneFile + World + camera. Disk-free, in-memory, single slot, overwritten each Play.
- *How to trigger?* — F5 hotkey, ▶/■ toolbar button, Esc-during-Play. Three independent triggers, all converge on the same `setMode` call.
- *Editor chrome during Play?* — Stays visible (matches Unity Scene view during Play). Toolbar + border visually indicate Play state.
- *What does the milestone *visually* demonstrate?* — A debug magenta falling cube. Proves physics integrates only during Play; the user's scene is untouched.
- *Where does snapshot/restore logic live?* — Sandbox (it owns `SceneFile` + `World` + camera). `EditorState` is just the flag.
