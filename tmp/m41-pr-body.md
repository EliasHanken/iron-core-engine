## Summary

M41 — Edit/Play mode separation for the sandbox editor. Foundation for every future runtime system: M42 CollisionShape, M43 AudioEmitter, scripting, AI all gate on `editor.isPlaying()`.

- **New module `engine/editor/EditorState.{h,cpp}`**: tiny `Mode` enum (`Edit`/`Play`) + `setMode` / `mode` / `isPlaying`. Thin by design — the host (sandbox) owns snapshot/restore.
- **`iron::World` deep-copy support**: `IComponentArray` gains a virtual `clone()`, `TypedComponentArray<T>` overrides it via the default copy ctor, `World` gains explicit copy ctor + copy assignment that walks `arrays_` calling `clone()`. Required because the sandbox snapshot/restore needs `editWorld = world;` to compile and produce a true deep copy. Unit-tested for deep-copy correctness + assignment overwrite + empty-component case.
- **Sandbox integration** (`games/11-sandbox/main.cpp`): construct `iron::PhysicsWorld` (M18), gate `physics.step` on Play, gate the entire scene-editing block on `!isPlaying`. Toolbar UI at top-center with `[>] Play` / `[#] Stop` button (F5 hotkey + Esc-during-Play also toggle). Orange viewport border + "PLAY MODE" text overlay in Play mode (via ImGui foreground draw list). AudioEngine wiring deferred to M43 (which actually has audio to listen to).
- **Snapshot/restore**: in-memory copies of `SceneFile` + `World` + `FreeFlyCamera` on Edit→Play, restored on Play→Edit. Single backup slot, overwritten each Play. User returns to their exact Edit pose on Stop.
- **Debug demonstration**: on Edit→Play, spawn a 1m³ dynamic Jolt cube at `(0, 5, 0)` + a 20×0.5×20 static floor. Rendered as a magenta wireframe (12 edges per box) via the M11 `Renderer::drawLineOverlay` API the sandbox already uses for selection outlines, sampling the body position each Play frame. Despawned on Play→Edit. Proves physics integrates only during Play.
- **Tests**: new `tests/test_editor_state.cpp` (3 named subtests covering initial mode, setMode flip, round-trip). `tests/test_world.cpp` gets 3 new subtests covering World deep-copy correctness, assignment overwrite, and empty-component case. 51 → 52 CTest entries.

## Test plan

- [x] Full suite green (52/52)
- [x] ironcore + ironcore_editor + sandbox build clean
- [x] Visual: Play button toggles mode + spawns magenta cube + viewport border. Stop restores scene + cam. F5 and Esc-during-Play also toggle.

## Known v1 limits (intentional, deferred)

- **Pause state** — only Play and Stop. Pause is genuinely useful but its snapshot/restore semantics are subtle; defer if needed.
- **Game-camera switching** — Play mode keeps the free-fly camera as a spectator view. Game camera waits for a `Player` concept.
- **Edit during Play** — the entire editing block is gated, so Inspector can't even change selection during Play. Refining to "Inspector visible-but-read-only" is a follow-up.
- **CollisionShape, AudioEmitter, scripting** — M42, M43, M46+. M41 is the substrate.
- **Disk-backed snapshot** — always in-memory.
- **Multi-snapshot / undo history** — single slot, overwritten each Play.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
