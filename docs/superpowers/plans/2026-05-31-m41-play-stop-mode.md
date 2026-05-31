# M41 — Play/Stop Mode Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Edit/Play mode separation to the sandbox editor. Edit mode = author the scene (current behavior). Play mode = simulate (physics steps, audio listener active, scene-editing disabled). Includes the EditorState mode flag, scene snapshot/restore, F5/Esc/toolbar triggers, and a debug falling cube to validate physics integrates only during Play.

**Architecture:** New thin `engine/editor/EditorState` class holds the `Mode` enum + flag. `iron::World` gains deep-copy support (virtual `clone()` on `IComponentArray` + explicit copy constructor on `World`). The sandbox owns the snapshot logic and the Play toolbar UI — `EditorState` only carries the flag. Physics stepping and the debug cube spawn/despawn gate on `editor.isPlaying()`.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), Dear ImGui (existing in `ironcore_editor`), `iron::PhysicsWorld` (Jolt-wrapped, M18), `iron::AudioEngine` (M26), `iron::World` (M37), `iron::GizmoRegistry::aabb` (M11). No new external dependencies. Reference spec: `docs/superpowers/specs/2026-05-31-m41-play-stop-mode-design.md`.

**Verification model:** Phases A1 + A2 are pure logic TDD (CTest). Phase B1 wires everything in the sandbox; its acceptance gate is "build clean + existing 52 tests stay green + sandbox launches" — no automated test for the integration shape, but the pure-logic substrate (EditorState mode flag + World copy correctness) IS unit-tested. Phase C1 (debug cube) is visual-gated. Phase D1 adds the user visual gate + PR + merge + memory.

**Build & test commands (used by every task):**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target ironcore_editor
cmake --build build-vk --config Debug --target test_editor_state    # after A1
cmake --build build-vk --config Debug --target test_world           # after A2
cmake --build build-vk --config Debug --target sandbox              # for B/C/D
ctest --test-dir build-vk -C Debug --output-on-failure
```
(A benign "LF will be replaced by CRLF" git warning is expected on Windows. Pre-existing ImGui/GLFW `LNK4217` linker warnings are benign.)

**Branch:** already on `feat/m41-play-stop-mode` (spec commit `8b90abc` sits on the branch tip). Every task in this plan commits to this branch.

---

## Current state of the codebase (read this before starting)

**`iron::World` (engine/world/World.h)** — stores components in `std::array<std::unique_ptr<IComponentArray>, 256> arrays_` + `std::vector<uint32_t> generations_` + `std::vector<uint32_t> freeList_`. The `unique_ptr` makes World **non-copyable by default**. Task A2 adds deep-copy support.

**`iron::PhysicsWorld` (engine/physics/PhysicsWorld.h)** — Jolt wrapper from M18. Public API used by M41: `init()`, `step(float dt)`, `createDynamicBox(Vec3 pos, Vec3 halfExtents, float mass) → BodyId`, `createStaticBox(Vec3 pos, Vec3 halfExtents) → BodyId`, `bodyPosition(BodyId) → Vec3`, `destroyBody(BodyId)`. Single-threaded externally; deterministic.

**`iron::Renderer::drawLineOverlay(Vec3 a, Vec3 b, Vec3 color)`** + `flushDebugLines(view, proj)` — the existing M11 debug-line API the sandbox already uses for the selection outline. M41 uses it to draw the debug cube as a wireframe (12 edges of an AABB) each Play frame.

**AudioEngine not used in M41** — the original spec mentioned wiring `audio.setListener(...)` per frame, but the sandbox doesn't currently construct an `AudioEngine` at all. Adding `AudioEngine` construction is its own small step that genuinely belongs with M43 (AudioEmitter), where there's actually audio to listen to. M41 skips it entirely.

**`iron::SceneFile` (engine/scene/SceneFormat.h)** — POD struct, value-copyable today (no unique_ptr / no virtual). Snapshot is a plain copy: `editScene = scene;`.

**`iron::FreeFlyCamera` (engine/scene/FreeFlyCamera.h)** — POD struct, value-copyable. Snapshot is a plain copy: `editCam = cam;`.

**`tests/test_framework.h`** — provides `CHECK(cond)` and `CHECK_NEAR(a, b)` macros, plus `iron_test_result()` returning 0 on success. M37+ reflection tests use an ad-hoc `CHECK` macro pattern instead; for consistency with the rest of the M41 work we use `test_framework.h`.

**Baseline:** 51 CTest cases as of M40 (PR #61, `43ec219`).

---

## File Structure

**New (engine):**
- `engine/editor/EditorState.h` — Mode enum + flag + accessors
- `engine/editor/EditorState.cpp` — empty body (everything inline) OR holds an out-of-line definition placeholder so the `ironcore_editor` link list has something to reference

**New (tests):**
- `tests/test_editor_state.cpp` — 3 named subtests (initial mode, setMode flip, round trip)

**Modified (engine):**
- `engine/world/World.h` — `IComponentArray::clone()` virtual + `TypedComponentArray::clone()` override + `World` copy constructor + copy assignment operator
- `engine/editor/CMakeLists.txt` — append `EditorState.cpp` to `ironcore_editor`
- `tests/CMakeLists.txt` — `iron_add_test(test_editor_state test_editor_state.cpp)` AND link `ironcore_editor` since the test pulls an editor header
- `tests/test_world.cpp` — append subtests for World copy correctness

**Modified (games):**
- `games/11-sandbox/main.cpp` — substantial: construct `EditorState`, `PhysicsWorld`; F5/Esc/toolbar handlers; per-frame `physics.step` gate; snapshot/restore on mode transitions; gate editor-input block on `!editor.isPlaying()`; debug cube spawn/despawn helpers; `renderer.drawLineOverlay` wireframe each Play frame

**Untouched on purpose:** Renderer (Vulkan), ReflectionInspector, ReflectionIO, SceneIO, ViewGizmo, Gizmo, picking, all shipping games (`net-shooter`, `02-strandbound`, etc.), reflection sidecars.

---

## Phases

- **A — Pure-logic engine additions** (TDD): EditorState module + tests; World deep-copy + tests. 2 tasks.
- **B — Sandbox integration**: PhysicsWorld + AudioEngine listener; F5/Esc/toolbar UI; snapshot/restore; editor-edit gating. 1 task.
- **C — Debug demonstration**: Falling cube + static floor; spawn on Edit→Play, destroy on Play→Edit; `updateAabb` each Play frame. 1 task.
- **D — Visual gate + push + PR + merge + memory**. 1 task.

Total: 5 tasks.

---

## Phase A — Pure-logic engine additions (TDD)

### Task A1: `engine/editor/EditorState.{h,cpp}` + `tests/test_editor_state.cpp`

**Files:**
- Create: `engine/editor/EditorState.h`
- Create: `engine/editor/EditorState.cpp`
- Create: `tests/test_editor_state.cpp`
- Modify: `engine/editor/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_editor_state.cpp`:

```cpp
#include "editor/EditorState.h"
#include "test_framework.h"

int main() {
    // --- Test 1: initial mode is Edit ---
    {
        iron::EditorState s;
        CHECK(s.mode() == iron::EditorState::Mode::Edit);
        CHECK(!s.isPlaying());
    }

    // --- Test 2: setMode(Play) flips isPlaying to true ---
    {
        iron::EditorState s;
        s.setMode(iron::EditorState::Mode::Play);
        CHECK(s.mode() == iron::EditorState::Mode::Play);
        CHECK(s.isPlaying());
    }

    // --- Test 3: Edit → Play → Edit round trip ---
    {
        iron::EditorState s;
        s.setMode(iron::EditorState::Mode::Play);
        s.setMode(iron::EditorState::Mode::Edit);
        CHECK(s.mode() == iron::EditorState::Mode::Edit);
        CHECK(!s.isPlaying());
    }

    return iron_test_result();
}
```

Add the test registration to `tests/CMakeLists.txt`. Append AFTER the existing `iron_add_test(test_iso_view ...)` line:

```cmake
add_executable(test_editor_state test_editor_state.cpp)
target_link_libraries(test_editor_state PRIVATE ironcore ironcore_editor)
target_include_directories(test_editor_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_editor_state COMMAND test_editor_state)
```

(Note: we use the manual `add_executable` form instead of the `iron_add_test` helper because `EditorState.h` lives in `ironcore_editor`, which the helper doesn't link by default.)

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_editor_state
```
Expected: compile error — `editor/EditorState.h` does not exist.

- [ ] **Step 3: Create `engine/editor/EditorState.h`**

```cpp
#pragma once

namespace iron {

// Two-state editor mode flag. Edit (default) = scene authoring, physics
// and runtime systems paused. Play = scene is simulated, runtime systems
// active. The flag itself is just bookkeeping — the host (sandbox) owns
// the snapshot/restore logic and gates physics/audio/scripting on
// isPlaying().
class EditorState {
public:
    enum class Mode { Edit, Play };

    Mode mode() const { return mode_; }
    bool isPlaying() const { return mode_ == Mode::Play; }

    void setMode(Mode m) { mode_ = m; }

private:
    Mode mode_ = Mode::Edit;
};

}  // namespace iron
```

- [ ] **Step 4: Create `engine/editor/EditorState.cpp`**

```cpp
#include "editor/EditorState.h"

// Everything in EditorState is inline in the header for now. This .cpp
// exists so CMake has a translation unit to compile for the ironcore_editor
// library — the link step needs at least one symbol from the file. Adding
// a touched-but-unused namespace-scope variable below keeps the TU
// non-empty without exposing anything.
namespace iron {
namespace { [[maybe_unused]] constexpr int kEditorStateTuMarker = 0; }
}  // namespace iron
```

- [ ] **Step 5: Register `EditorState.cpp` in `engine/editor/CMakeLists.txt`**

Edit `engine/editor/CMakeLists.txt`. Append `EditorState.cpp` to the `add_library(ironcore_editor STATIC ...)` source list (anywhere; alphabetical doesn't strictly matter):

```cmake
add_library(ironcore_editor STATIC
  Gizmo.cpp
  ImGuiLayer.cpp
  SceneOutliner.cpp
  SceneInspector.cpp
  EnvironmentPanel.cpp
  ReflectionInspector.cpp
  ViewGizmo.cpp
  EditorState.cpp
)
```

- [ ] **Step 6: Build + run tests**

```bash
cmake --build build-vk --config Debug --target test_editor_state
cd build-vk && ctest -C Debug -R test_editor_state --output-on-failure -V
```
Expected: `OK - all checks passed`. CTest summary: 1/1 Passed.

Also verify the full suite still passes:

```bash
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 52/52 green (51 prior + new test_editor_state).

- [ ] **Step 7: Commit**

```bash
git add engine/editor/EditorState.h engine/editor/EditorState.cpp engine/editor/CMakeLists.txt tests/test_editor_state.cpp tests/CMakeLists.txt
git commit -m "M41: EditorState — Mode enum + flag + tests"
```

Run `git log --oneline -3` and include the SHA in your report.

---

### Task A2: `iron::World` deep-copy support + tests

**Files:**
- Modify: `engine/world/World.h`
- Modify: `tests/test_world.cpp`

`iron::World` currently can't be copied because `arrays_` holds `std::unique_ptr<IComponentArray>`. The sandbox's snapshot/restore in Task B1 needs `editWorld = world;` to compile and produce a true deep copy. We add a virtual `clone()` on `IComponentArray`, override it in `TypedComponentArray<T>`, then give `World` an explicit copy constructor + copy assignment.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_world.cpp` BEFORE the final `return g_failures == 0 ? 0 : 1;` (which sits inside `main()`). Find the closing `return` of `main()` and insert these test functions ABOVE `main()`, then call them from within `main()` before the return:

```cpp
struct ComponentA { int value = 0; };
struct ComponentB { float value = 0.0f; };

static void test_world_copy_constructor_deep_copies_components() {
    iron::World original;
    const iron::EntityId e = original.create();
    original.add<ComponentA>(e, ComponentA{42});
    original.add<ComponentB>(e, ComponentB{3.14f});

    iron::World copy(original);

    // The copy sees the same component values.
    CHECK(copy.alive(e));
    CHECK(copy.get<ComponentA>(e) != nullptr);
    CHECK(copy.get<ComponentA>(e)->value == 42);
    CHECK(copy.get<ComponentB>(e) != nullptr);
    CHECK(copy.get<ComponentB>(e)->value == 3.14f);

    // Mutating the copy does NOT affect the original (deep copy, not shared).
    copy.get<ComponentA>(e)->value = 99;
    CHECK(original.get<ComponentA>(e)->value == 42);
    CHECK(copy.get<ComponentA>(e)->value == 99);
}

static void test_world_copy_assignment_overwrites_target() {
    iron::World source;
    const iron::EntityId e = source.create();
    source.add<ComponentA>(e, ComponentA{7});

    iron::World target;
    const iron::EntityId existingInTarget = target.create();
    target.add<ComponentA>(existingInTarget, ComponentA{1234});

    target = source;

    // After assignment, `target` mirrors `source`. The pre-existing entity
    // in target should be gone (or at least its ComponentA isn't 1234 anymore).
    CHECK(target.alive(e));
    CHECK(target.get<ComponentA>(e) != nullptr);
    CHECK(target.get<ComponentA>(e)->value == 7);
}

static void test_world_copy_handles_empty_components() {
    iron::World original;   // never adds anything
    iron::World copy(original);
    iron::EntityId e = copy.create();
    CHECK(copy.alive(e));   // copy still works
}
```

In `main()`, call these tests after the existing calls (find where `test_component_array_add_and_get` or similar is called — there should be a sequence of `test_xxx()` calls before `return`). Append:

```cpp
    test_world_copy_constructor_deep_copies_components();
    test_world_copy_assignment_overwrites_target();
    test_world_copy_handles_empty_components();
```

- [ ] **Step 2: Run the failing test**

```bash
cmake --build build-vk --config Debug --target test_world
```
Expected: compile error — `iron::World copy(original);` fails because World's implicit copy constructor was deleted (due to `unique_ptr` member).

- [ ] **Step 3: Add deep-copy support in `engine/world/World.h`**

Edit `engine/world/World.h`. Find the `IComponentArray` struct (around line 48) and add a virtual `clone()`:

Old:
```cpp
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void remove(EntityId e) = 0;   // type-erased remove for destroy()
    };
```

New:
```cpp
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void remove(EntityId e) = 0;   // type-erased remove for destroy()
        virtual std::unique_ptr<IComponentArray> clone() const = 0;
    };
```

Find the `TypedComponentArray` struct (right below) and add the override:

Old:
```cpp
    template <class T>
    struct TypedComponentArray : IComponentArray, ComponentArray<T> {
        void remove(EntityId e) override { ComponentArray<T>::remove(e); }
    };
```

New:
```cpp
    template <class T>
    struct TypedComponentArray : IComponentArray, ComponentArray<T> {
        void remove(EntityId e) override { ComponentArray<T>::remove(e); }
        std::unique_ptr<IComponentArray> clone() const override {
            return std::make_unique<TypedComponentArray<T>>(*this);
        }
    };
```

Now add the copy constructor + copy assignment to `World` (these go in the `public:` section, after `bool alive(EntityId e) const;` — around line 20):

```cpp
    // Deep copy. Snapshot/restore in editors (M41 Play/Stop) and tests rely
    // on this. Each ComponentArray is cloned via the virtual clone() above;
    // generations_ + freeList_ are POD-vector copies.
    World() = default;
    World(const World& other) { copyFrom(other); }
    World& operator=(const World& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }
    World(World&&) = default;
    World& operator=(World&&) = default;
```

Add the `copyFrom` helper in the `private:` section (right after `tryArrayFor`):

```cpp
    void copyFrom(const World& other) {
        for (uint32_t i = 0; i < kMaxComponentTypes; ++i) {
            arrays_[i] = other.arrays_[i] ? other.arrays_[i]->clone() : nullptr;
        }
        generations_ = other.generations_;
        freeList_    = other.freeList_;
    }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_world
cd build-vk && ctest -C Debug -R test_world --output-on-failure -V
```
Expected: clean build, `test_world` passes (all prior assertions + the 3 new ones).

Also verify the full suite stays green:

```bash
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 52/52 green.

- [ ] **Step 5: Commit**

```bash
git add engine/world/World.h tests/test_world.cpp
git commit -m "M41: World deep-copy support (IComponentArray::clone + copy ctor)"
```

---

## Phase B — Sandbox integration

### Task B1: PhysicsWorld + AudioEngine listener + F5/Esc/toolbar + snapshot/restore + editor-edit gating

**Files:**
- Modify: `games/11-sandbox/main.cpp`

This is the big integration task. We're touching ~10 different spots in main.cpp. Read the change list before starting; the steps walk through each insertion point sequentially.

The high-level shape of what we're adding to `main()`:
1. After existing camera / window setup: construct `iron::EditorState editor`
2. Construct `iron::PhysicsWorld physics; physics.init();`
3. Reserve three snapshot locals: `iron::SceneFile editScene; iron::World editWorld; iron::FreeFlyCamera editCam;`
4. In setUpdate, near the top: F5 keypress + Esc-during-Play handling that calls a `togglePlayMode(...)` lambda
5. In setUpdate: if `editor.isPlaying()` then `physics.step(t.deltaSeconds)`. No AudioEngine wiring in M41 (deferred to M43).
6. In setUpdate: wrap the scene-editing block (picking + gizmo.update + add/delete) with `if (!editor.isPlaying()) { ... }`
7. In setRender: draw the Play/Stop toolbar (small ImGui window) BEFORE the existing inspector/outliner panels. While `editor.isPlaying()`, draw a colored border + "PLAY MODE" overlay via ImGui::GetForegroundDrawList.

The `togglePlayMode` lambda body (defined where the snapshot locals live):
- If currently Edit → snapshot scene/world/cam, setMode(Play), call `spawnDebugBodies()` (the Task C1 hook)
- If currently Play → call `despawnDebugBodies()`, restore scene/world/cam, setMode(Edit)

Task C1 fills in `spawnDebugBodies` and `despawnDebugBodies`. In B1, define them as empty lambdas/functions so the toggle is testable in isolation.

- [ ] **Step 1: Add includes near the top of `games/11-sandbox/main.cpp`**

Find the existing engine include block (top of file, around lines 20–50). Add these alongside the other `editor/` and `physics/` includes:

```cpp
#include "editor/EditorState.h"
#include "physics/PhysicsWorld.h"
```

(`engine/world/World.h` is already included via earlier M37 work. Same for `engine/audio/AudioEngine.h` from M26.)

- [ ] **Step 2: Construct editor state, physics world, and snapshot locals in `main()`**

In `main()`, after the existing camera setup block (look for `iron::FreeFlyCamera cam;` around line 441), add the M41 state:

```cpp
    // M41: Play/Stop mode state.
    iron::EditorState editor;
    iron::PhysicsWorld physics;
    if (!physics.init()) {
        iron::Log::error("sandbox: PhysicsWorld init failed");
        return 1;
    }
    // Snapshot slots (populated on Edit→Play, consumed on Play→Edit).
    iron::SceneFile     editScene;
    iron::World         editWorld;
    iron::FreeFlyCamera editCam;
    // Debug-cube body handles (populated by spawnDebugBodies in Task C1; stay
    // kInvalidBody during Edit mode). No persistent gizmo needed — Task C1
    // emits drawLineOverlay calls each Play frame for the wireframe.
    iron::BodyId debugCubeBody  = iron::kInvalidBody;
    iron::BodyId debugFloorBody = iron::kInvalidBody;
```

Then define the spawn/despawn hooks + toggle right below (these are LAMBDAS capturing by reference, so they see all the state above):

```cpp
    // Spawn/despawn the debug body pair. Filled in by Task C1.
    auto spawnDebugBodies = [&]() {
        // Task C1 fills this in with the falling-cube + static-floor pair.
    };
    auto despawnDebugBodies = [&]() {
        // Task C1 destroys the bodies + gizmo. In B1 this is a no-op.
    };

    auto togglePlayMode = [&]() {
        if (editor.isPlaying()) {
            // Play → Edit: tear down debug bodies, restore snapshot.
            despawnDebugBodies();
            scene = editScene;
            world = editWorld;
            cam   = editCam;
            editor.setMode(iron::EditorState::Mode::Edit);
            iron::Log::info("sandbox: Play → Edit");
        } else {
            // Edit → Play: snapshot, spawn debug bodies.
            editScene = scene;
            editWorld = world;
            editCam   = cam;
            editor.setMode(iron::EditorState::Mode::Play);
            spawnDebugBodies();
            iron::Log::info("sandbox: Edit → Play");
        }
    };
```

- [ ] **Step 3: Add per-frame physics step in setUpdate**

Find the start of the existing setUpdate lambda (look for `app.setUpdate([&](const iron::FrameTime& t) {`, around line 534). After the resize/minimize guards (around line 547), but BEFORE the existing camera input block, insert:

```cpp
        // M41: physics runs only in Play mode.
        if (editor.isPlaying()) {
            physics.step(t.deltaSeconds);
        }
```

(`AudioEngine::setListener` was in the original spec but the sandbox doesn't construct an `AudioEngine`; adding it is its own small task that belongs with M43.)

- [ ] **Step 4: Add F5 + Esc-during-Play handlers**

In setUpdate, near the existing Esc handler (search for `GLFW_KEY_ESCAPE` — around line 555). Replace:

Old:
```cpp
        if (input.keyPressed(GLFW_KEY_ESCAPE))
            selectedIndex = -1;
```

New:
```cpp
        // M41: F5 toggles Play/Edit unconditionally. Esc in Play mode exits
        // to Edit; Esc in Edit mode keeps its existing "deselect" behaviour.
        if (input.keyPressed(GLFW_KEY_F5)) {
            togglePlayMode();
        }
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            if (editor.isPlaying()) {
                togglePlayMode();
            } else {
                selectedIndex = -1;
            }
        }
```

- [ ] **Step 5: Gate the scene-editing block on `!editor.isPlaying()`**

Find the existing block that does picking + gizmo + add/delete + Inspector. The exact bounds are nontrivial because the editing logic is spread across several sub-blocks. The safest gate is: wrap the entire region from "click-to-select / gizmo update" through "save scene shortcut / add-cube shortcut / duplicate shortcut / delete shortcut / Inspector edit propagation" with one `if`.

The simplest robust implementation: identify the FIRST line of editing logic (likely `const bool uiBusy = imgui.wantsMouse();` around line 602 OR the picking ray construction around line 661) and the LAST line (likely the Inspector edit application or the duplicate shortcut). Wrap both in:

```cpp
        if (!editor.isPlaying()) {
            // ... existing editing block ...
        }
```

If the editing logic is interleaved with rendering decisions, narrow the gate to ONLY the mutating calls:
- `gizmo.update(...)` — skip when playing
- The `selectedIndex = ...` reassignment from picking — keep (selection is read-only)
- Inspector edit application (the `inspector.draw(...) → bool changed` branch) — skip the write-back when playing
- Add/delete/duplicate shortcuts — skip when playing

**Pragmatic v1 implementation:** in B1, wrap the LARGE editing block from picking-or-gizmo through the add/delete shortcuts with one `if (!editor.isPlaying())`. This may also disable picking-selection during Play (user can't click an entity to see it in the Inspector) — that's acceptable for M41 and can be refined in a follow-up. Inspector remains visible but the user can't change the selection.

Concretely: find the block starting with `const bool uiBusy = imgui.wantsMouse();` (around line 602) and ending after the add/delete/duplicate shortcut handlers (look for the last `wantDuplicateShortcut` or similar — around line 720). Wrap with:

```cpp
        if (!editor.isPlaying()) {
            const bool uiBusy = imgui.wantsMouse();
            // ... entire editing block as it is now ...
        }
```

(Use your editor's block-fold to find the exact start/end lines. Match indentation; the existing block uses 8 spaces.)

- [ ] **Step 6: Add the Play/Stop toolbar UI in setRender**

Find the start of the setRender lambda. Search for `app.setRender([&]() {` (around line 664). After `imgui.beginFrame();` (around line 673) and BEFORE the existing panel draws (`outliner.draw(...)`, `inspector.draw(...)`, etc.), add the toolbar:

```cpp
        // M41: Play/Stop toolbar. Top-center small ImGui window.
        {
            const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            constexpr float kToolbarW = 130.0f;
            constexpr float kToolbarH = 38.0f;
            ImGui::SetNextWindowPos({vpPos.x + (vpSize.x - kToolbarW) * 0.5f,
                                     vpPos.y + 8.0f});
            ImGui::SetNextWindowSize({kToolbarW, kToolbarH});
            constexpr ImGuiWindowFlags kToolbarFlags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav;
            ImGui::Begin("##play_toolbar", nullptr, kToolbarFlags);
            const char* label = editor.isPlaying() ? "[#] Stop" : "[>] Play";
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f))) {
                togglePlayMode();
            }
            ImGui::End();
        }

        // M41: "PLAY MODE" banner + colored border around the whole viewport.
        if (editor.isPlaying()) {
            const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            // 4px-thick orange border.
            constexpr ImU32 kPlayCol = IM_COL32(255, 140, 0, 220);
            const ImVec2 a{vpPos.x, vpPos.y};
            const ImVec2 b{vpPos.x + vpSize.x, vpPos.y + vpSize.y};
            fg->AddRect(a, b, kPlayCol, 0.0f, 0, 4.0f);
            // "PLAY MODE" text top-right (above the view-gizmo).
            const char* msg = "PLAY MODE";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            fg->AddText({b.x - ts.x - 16.0f, a.y + 16.0f}, kPlayCol, msg);
        }
```

(The `[>]` / `[#]` are ASCII stand-ins for "play" / "stop" glyphs. Plain text avoids font-icon-dependency questions. If you want pretty triangle/square icons later, swap the label.)

- [ ] **Step 7: Build the sandbox + run all tests**

```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build (benign LNK4217 warnings only); 52/52 tests green.

Do NOT launch the sandbox yet — Task C1 adds the debug cube demo, then D1 has the user visual gate.

- [ ] **Step 8: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M41: sandbox plumbing — PhysicsWorld + audio listener + F5/Esc/toolbar + snapshot/restore + edit gating"
```

---

## Phase C — Debug demonstration

### Task C1: Debug falling cube + static floor

**Files:**
- Modify: `games/11-sandbox/main.cpp`

In Task B1 we defined `spawnDebugBodies` and `despawnDebugBodies` as empty hooks. Task C1 fills them in. Plus add a per-frame `updateAabb` call so the rendered cube tracks the Jolt body's position during Play.

- [ ] **Step 1: Fill in `spawnDebugBodies`**

Find the existing empty lambda in `main()` (added in B1 Step 2):

Old:
```cpp
    auto spawnDebugBodies = [&]() {
        // Task C1 fills this in with the falling-cube + static-floor pair.
    };
```

New:
```cpp
    auto spawnDebugBodies = [&]() {
        // Falling demo cube — 1m³, mass 5kg, dropped from above the origin.
        debugCubeBody = physics.createDynamicBox(
            iron::Vec3{0.0f, 5.0f, 0.0f},   // position
            iron::Vec3{0.5f, 0.5f, 0.5f},   // half-extents
            5.0f);                          // mass
        // Static floor — 20×0.5×20, top face at y=0 so the cube lands at y=0.5.
        debugFloorBody = physics.createStaticBox(
            iron::Vec3{0.0f, -0.25f, 0.0f},   // position
            iron::Vec3{10.0f, 0.25f, 10.0f}); // half-extents
    };
```

- [ ] **Step 2: Fill in `despawnDebugBodies`**

Find:
```cpp
    auto despawnDebugBodies = [&]() {
        // Task C1 destroys the bodies + gizmo. In B1 this is a no-op.
    };
```

Replace with:
```cpp
    auto despawnDebugBodies = [&]() {
        if (debugCubeBody.isValid())  physics.destroyBody(debugCubeBody);
        if (debugFloorBody.isValid()) physics.destroyBody(debugFloorBody);
        debugCubeBody  = iron::kInvalidBody;
        debugFloorBody = iron::kInvalidBody;
    };
```

- [ ] **Step 3: Render the debug cube as a wireframe each Play frame**

In setRender (NOT setUpdate — drawLineOverlay submits to the debug-line buffer that gets flushed at the end of the frame's render pass), find the existing call to `renderer.flushDebugLines(view, proj);` (around line 893). Immediately BEFORE that flush call, add:

```cpp
        // M41: draw the debug falling cube as a magenta wireframe, tracking
        // the Jolt body each Play frame. No-op in Edit mode.
        if (editor.isPlaying() && debugCubeBody.isValid()) {
            const iron::Vec3 p = physics.bodyPosition(debugCubeBody);
            const iron::Vec3 magenta{1.0f, 0.2f, 0.8f};
            constexpr float h = 0.5f;  // half-extent
            // 8 corners of the AABB centered at p.
            const iron::Vec3 c[8] = {
                {p.x - h, p.y - h, p.z - h}, {p.x + h, p.y - h, p.z - h},
                {p.x + h, p.y + h, p.z - h}, {p.x - h, p.y + h, p.z - h},
                {p.x - h, p.y - h, p.z + h}, {p.x + h, p.y - h, p.z + h},
                {p.x + h, p.y + h, p.z + h}, {p.x - h, p.y + h, p.z + h},
            };
            // 12 edges of a box: 4 bottom + 4 top + 4 verticals.
            const int e[12][2] = {
                {0,1},{1,2},{2,3},{3,0},
                {4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7},
            };
            for (const auto& edge : e) {
                renderer.drawLineOverlay(c[edge[0]], c[edge[1]], magenta);
            }
            // Also draw the floor outline so the user can see what's catching
            // the cube. Floor is 20×0.5×20 centered at (0, -0.25, 0).
            const iron::Vec3 fmin{-10.0f, -0.5f, -10.0f};
            const iron::Vec3 fmax{ 10.0f,  0.0f,  10.0f};
            const iron::Vec3 floorMagenta{0.6f, 0.15f, 0.5f};  // dimmer
            const iron::Vec3 f[8] = {
                {fmin.x, fmin.y, fmin.z}, {fmax.x, fmin.y, fmin.z},
                {fmax.x, fmax.y, fmin.z}, {fmin.x, fmax.y, fmin.z},
                {fmin.x, fmin.y, fmax.z}, {fmax.x, fmin.y, fmax.z},
                {fmax.x, fmax.y, fmax.z}, {fmin.x, fmax.y, fmax.z},
            };
            for (const auto& edge : e) {
                renderer.drawLineOverlay(f[edge[0]], f[edge[1]], floorMagenta);
            }
        }
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; 52/52 tests green.

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M41: debug falling-cube + static-floor demo on Edit→Play"
```

---

## Phase D — Verification + PR + merge

### Task D1: Visual gate + push + PR + squash-merge + memory

- [ ] **Step 1: Full clean build + tests**

```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 52/52 green.

- [ ] **Step 2: User visual gate**

Hand back to the user with this checklist:

```
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

| Action | Expected |
|---|---|
| Open sandbox | Top-center: small toolbar with `[>] Play` button. No PLAY MODE banner yet. |
| Click Play (or F5) | Button changes to `[#] Stop`. Orange border appears around viewport. "PLAY MODE" text top-right. A magenta wireframe cube falls from above the origin and lands on a dimmer-magenta wireframe floor. |
| Try to drag the entity gizmo on the helmet | Gizmo doesn't drag (editing is gated in Play). |
| Try to click another entity in the scene | Selection doesn't change (whole editing block is gated). |
| WASD + RMB-look | Camera moves freely (camera affordances stay enabled). |
| View-gizmo (axis cube), Iso button, MMB orbit, wheel zoom | All work — camera affordances unaffected by Play mode. |
| Click Stop (or F5 or Esc) | Magenta cube + border vanish. Camera + scene state restored to exactly where they were before Play. |
| Click Play again | Cube spawns again at `(0, 5, 0)` and falls again. |
| Save scene (existing hotkey) while in Play mode | Should NOT trigger (it's part of the gated editing block). If it does fire, the save is harmless — but the gate should prevent it. |

If any row regresses, return to B1 (gating) or C1 (debug bodies) and fix before proceeding.

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/m41-play-stop-mode
```

- [ ] **Step 4: Open the PR**

Write the PR body to a file first to avoid PowerShell here-string quoting issues:

```bash
mkdir -p tmp
```

Create `tmp/m41-pr-body.md`:

```markdown
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

- **Pause state** — only Play and Stop. Pause is genuinely useful but its snapshot/restore semantics are subtle; defer to M41.5 if needed.
- **Game-camera switching** — Play mode keeps the free-fly camera as a spectator view. Game camera waits for a `Player` concept.
- **Edit during Play** — the entire editing block is gated, so Inspector can't even change selection during Play. Refining to "Inspector visible-but-read-only" is a follow-up.
- **CollisionShape, AudioEmitter, scripting** — M42, M43, M46+. M41 is the substrate.
- **Disk-backed snapshot** — always in-memory.
- **Multi-snapshot / undo history** — single slot, overwritten each Play.

EOF (for the gh CLI)
```

Open the PR with the body from the file:

```bash
gh pr create --title "M41: Edit/Play mode infrastructure + World deep-copy + debug demo" --body-file tmp/m41-pr-body.md
```

- [ ] **Step 5: Watch CI**

```bash
gh pr checks --watch
```

Expected: `Build & test (Windows / MSVC)` passes within ~5 minutes (warm cache).

If CI fails on a transient issue (vcpkg / Kitware mirror flake), re-run via `gh run rerun <run-id> --failed`.

- [ ] **Step 6: Squash-merge**

When CI is green:

```bash
gh pr merge --squash --delete-branch
git checkout main && git pull --ff-only origin main
git log --oneline -3
```

- [ ] **Step 7: Update memory**

After merge, update three files:

- In `MEMORY.md` index entry for `iron-core-engine-progress`: bump the SHA + milestone line to note M41 merged. Mention M42 (CollisionShape) as the next milestone.
- In `iron-core-engine-progress.md`: append an `M41 — Play/Stop mode infrastructure` entry near the M40 entry with the merge SHA, a one-paragraph summary, the v1 limits list from the PR body, and a callout that this was the foundation milestone that future runtime-system milestones (M42 CollisionShape, M43 AudioEmitter, M46+ scripting) gate on.
- In `iron-core-engine-roadmap.md`: mark M41 done; update the "next options after M40" line to drop M41 from the options and surface M42 (CollisionShape) as the foundation/authoring-track next step.

---

## Acceptance criteria

1. `iron::EditorState::mode()` defaults to `Mode::Edit`; `setMode(Mode::Play)` flips it; `isPlaying()` returns the right value in both states.
2. `iron::World` is now copy-constructible AND copy-assignable; both produce true deep copies (mutating the copy doesn't affect the original).
3. Sandbox toolbar shows `[>] Play` in Edit mode, `[#] Stop` + orange border + "PLAY MODE" text overlay in Play mode.
4. F5 toggles modes from either state. Esc in Play mode exits to Edit; Esc in Edit mode keeps the prior "deselect" behavior.
5. While in Play mode, the magenta wireframe debug cube falls from `(0, 5, 0)` and lands on the dimmer-magenta wireframe floor (rendered via `renderer.drawLineOverlay` each frame, sampling `physics.bodyPosition(debugCubeBody)`).
6. Toggling Play → Stop → Play in the same session reuses the snapshot mechanism cleanly (cube respawns at `(0, 5, 0)`, no leftover state).
7. `scene.entities` (floor / cubes / helmet from `demo.json`) are NOT affected by Play mode — positions/rotations/scales exactly the same after Play→Stop as before Play.
8. Camera position / yaw / pitch / fovDeg are exactly restored on Play→Stop.
9. Inspector edits, gizmo drags, and add/delete/duplicate are silently disabled in Play mode (no crashes, no spurious mutations).
10. WASD + RMB-look + view-gizmo + Iso button + MMB orbit + wheel zoom continue to work in Play mode.
11. 51 → 52 CTest cases green.
12. Renderer / shipping games / reflection layer untouched.

---

## Risk log

- **`World` copy constructor depth** — if `ComponentArray<T>::dense_` holds anything with internal pointers (e.g. a future `std::unique_ptr<T>`-typed component), the simple value copy in `TypedComponentArray::clone()` won't deep-copy them. Today no component does this; flag this risk when the first such component lands.
- **Edit-block gate boundaries** — wrapping the entire editing region with one `if` is the safe call but may inadvertently disable selection-only behaviors that ARE safe during Play. If a follow-up wants finer-grained gating (e.g. allow click-to-select but disable gizmo drag), refactor in M41.1.
- **F5 collision** — F5 isn't currently bound in the sandbox to anything else, but if some future milestone adds another F5 handler, the order of checks matters. Documented in the inline comment.
- **`spawnDebugBodies` race** — if the user toggles Play→Edit→Play very quickly (within one frame), the cube spawns twice. Mitigated by `togglePlayMode` checking current state first, but worth manual eyeball during D1's visual gate.
