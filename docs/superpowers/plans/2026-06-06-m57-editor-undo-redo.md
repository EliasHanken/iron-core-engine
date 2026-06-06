# M57 — Editor Undo/Redo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Ctrl+Z / Ctrl+Y "smart history" to the editor across both the scene (entity edits) and the node graph, where one user gesture = one undo step.

**Architecture:** A generic, ImGui-free `UndoHistory` (snapshot/memento over opaque strings) is instantiated once per document. The host owns two — scene + graph — and supplies each document's serialize/restore. Scene serializes via new in-memory `SceneIO` string helpers and restores by rebuilding the derived mirrors (World + resolved + index map). The graph reuses its existing `toJson()`/`loadFromJson()`. A per-document coalescing rule (open a transaction when the serialized state first differs, commit when the document is stable and the user isn't mid-interaction) collapses a whole drag into one entry.

**Tech Stack:** C++17, nlohmann/json (vendored), ImGui 1.92.8 + imgui-node-editor, CTest. Canonical build dir `build-vk` (Vulkan). Build the whole tree (not incremental) before claiming CI-ready.

---

## File Structure

- **`engine/editor/UndoHistory.h` / `.cpp`** (NEW) — the generic snapshot stack. ImGui-free, lives in the `ironcore_editor` lib alongside the other editor modules. One responsibility: store before-snapshots, hand back the snapshot to restore to on undo/redo.
- **`tests/test_undo_history.cpp`** (NEW) — headless unit test of the stack with plain strings.
- **`engine/scene/SceneIO.h` / `.cpp`** (MODIFY) — add `sceneToJsonString` / `sceneFromJsonString`; refactor the existing file helpers to call them so there's exactly one serializer.
- **`tests/test_scene_io.cpp`** (MODIFY) — add a direct in-memory round-trip assertion.
- **`engine/editor/NodeGraphPanel.h` / `.cpp`** (MODIFY) — expose whether the Node Editor window is focused, for shortcut routing.
- **`games/11-sandbox/main.cpp`** (MODIFY) — two `UndoHistory` instances wrapped in a small `DocHistory` helper; a `rebuildDerivedFromScene()` lambda; baseline init; clear-on-Play; Ctrl+Z/Y latching + focus-routed apply; per-frame coalescing.
- **`engine/editor/CMakeLists.txt`** (MODIFY) — add `UndoHistory.cpp` to `ironcore_editor`.
- **`tests/CMakeLists.txt`** (MODIFY) — register `test_undo_history` + `test_scene_io` already present.

---

### Task 1: Generic `UndoHistory` snapshot stack

**Files:**
- Create: `engine/editor/UndoHistory.h`
- Create: `engine/editor/UndoHistory.cpp`
- Test: `tests/test_undo_history.cpp`
- Modify: `engine/editor/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `engine/editor/UndoHistory.h`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iron {

// Generic, document-agnostic undo/redo over opaque string snapshots. A document
// (scene, node graph, …) serializes itself to a string; this class stores the
// pre-edit snapshots and hands back the snapshot to restore to on undo/redo. It
// knows nothing about what the strings contain — instantiate one per document.
//
// Model: the undo stack holds states the user can step BACK to. The current
// state is supplied at undo/redo time (not stored), so redo is exact without a
// sentinel "present" entry.
class UndoHistory {
public:
    explicit UndoHistory(std::size_t capacity = 100);

    // Push a pre-edit snapshot as a new undo entry. Clears the redo stack.
    // Evicts the oldest entry if capacity is exceeded.
    void commit(std::string beforeSnapshot);

    bool canUndo() const;
    bool canRedo() const;

    // Given the document's CURRENT serialized state, return the snapshot to
    // restore to, and record `current` on the opposite stack. Returns nullopt
    // (no-op) when the relevant stack is empty.
    std::optional<std::string> undo(const std::string& current);
    std::optional<std::string> redo(const std::string& current);

    void clear();

private:
    std::vector<std::string> undo_;
    std::vector<std::string> redo_;
    std::size_t capacity_;
};

}  // namespace iron
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_undo_history.cpp`:

```cpp
#include "editor/UndoHistory.h"
#include "test_framework.h"

using namespace iron;

int main() {
    // commit -> undo returns the committed (before) snapshot; redo returns the
    // current state captured at undo time.
    {
        UndoHistory h;
        CHECK(!h.canUndo());
        CHECK(!h.canRedo());
        h.commit("A");                 // before-edit state was "A"; doc is now "B"
        CHECK(h.canUndo());
        auto u = h.undo("B");          // step back from "B" to "A"
        CHECK(u.has_value());
        CHECK(*u == "A");
        CHECK(h.canRedo());
        auto r = h.redo("A");          // step forward from "A" back to "B"
        CHECK(r.has_value());
        CHECK(*r == "B");
        CHECK(!h.canRedo());
    }

    // A fresh commit clears the redo stack.
    {
        UndoHistory h;
        h.commit("A");
        (void)h.undo("B");             // now redo has "B"
        CHECK(h.canRedo());
        h.commit("C");                 // new edit -> redo cleared
        CHECK(!h.canRedo());
    }

    // Empty-stack undo/redo are no-ops.
    {
        UndoHistory h;
        CHECK(!h.undo("X").has_value());
        CHECK(!h.redo("X").has_value());
    }

    // Capacity eviction: oldest entry is dropped; can still undo, but not past
    // the cap.
    {
        UndoHistory h(2);
        h.commit("s0");
        h.commit("s1");
        h.commit("s2");                // "s0" evicted; stack holds [s1, s2]
        auto a = h.undo("cur");
        CHECK(*a == "s2");
        auto b = h.undo("s2");
        CHECK(*b == "s1");
        CHECK(!h.canUndo());           // only 2 retained
    }

    // Round-trip invariant: undo then redo returns the same current state.
    {
        UndoHistory h;
        h.commit("before");
        auto u = h.undo("after");
        CHECK(*u == "before");
        auto r = h.redo("before");
        CHECK(*r == "after");
    }

    return 0;
}
```

- [ ] **Step 3: Register the test and the source in CMake**

In `engine/editor/CMakeLists.txt`, add `UndoHistory.cpp` to the `ironcore_editor` source list (after `EditorState.cpp`):

```cmake
add_library(ironcore_editor STATIC
  Gizmo.cpp
  ImGuiLayer.cpp
  SceneOutliner.cpp
  SceneInspector.cpp
  EnvironmentPanel.cpp
  NodeGraphPanel.cpp
  ReflectionInspector.cpp
  ViewGizmo.cpp
  EditorState.cpp
  UndoHistory.cpp
)
```

In `tests/CMakeLists.txt`, register the test linking `ironcore_editor` (mirror `test_editor_state`, after that block near line 131):

```cmake
add_executable(test_undo_history test_undo_history.cpp)
target_link_libraries(test_undo_history PRIVATE ironcore ironcore_editor)
target_include_directories(test_undo_history PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_undo_history COMMAND test_undo_history)
```

- [ ] **Step 4: Run the test to verify it fails (no implementation yet)**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_undo_history --config Debug
```
Expected: FAIL — link error "unresolved external symbol" for `UndoHistory::commit`/`undo`/`redo`/etc. (header exists, `.cpp` doesn't yet).

- [ ] **Step 5: Write the implementation**

Create `engine/editor/UndoHistory.cpp`:

```cpp
#include "editor/UndoHistory.h"

#include <utility>

namespace iron {

UndoHistory::UndoHistory(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void UndoHistory::commit(std::string beforeSnapshot) {
    undo_.push_back(std::move(beforeSnapshot));
    if (undo_.size() > capacity_)
        undo_.erase(undo_.begin());   // evict the oldest
    redo_.clear();
}

bool UndoHistory::canUndo() const { return !undo_.empty(); }
bool UndoHistory::canRedo() const { return !redo_.empty(); }

std::optional<std::string> UndoHistory::undo(const std::string& current) {
    if (undo_.empty()) return std::nullopt;
    redo_.push_back(current);
    std::string snapshot = std::move(undo_.back());
    undo_.pop_back();
    return snapshot;
}

std::optional<std::string> UndoHistory::redo(const std::string& current) {
    if (redo_.empty()) return std::nullopt;
    undo_.push_back(current);
    std::string snapshot = std::move(redo_.back());
    redo_.pop_back();
    return snapshot;
}

void UndoHistory::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace iron
```

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_undo_history --config Debug && ctest --test-dir build-vk -C Debug -R test_undo_history --output-on-failure
```
Expected: PASS — `test_undo_history` 1/1 passes.

- [ ] **Step 7: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/editor/UndoHistory.h engine/editor/UndoHistory.cpp tests/test_undo_history.cpp engine/editor/CMakeLists.txt tests/CMakeLists.txt && git commit -F tmp/commit-msg.txt
```
(Write the message to `tmp/commit-msg.txt` first — single-quoted, ending with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer — then `rm -f tmp/commit-msg.txt`. Suggested subject: `M57: generic UndoHistory snapshot stack + unit test`.)

---

### Task 2: In-memory SceneIO string helpers

**Files:**
- Modify: `engine/scene/SceneIO.h`
- Modify: `engine/scene/SceneIO.cpp`
- Test: `tests/test_scene_io.cpp`

- [ ] **Step 1: Declare the new helpers in the header**

In `engine/scene/SceneIO.h`, after the `saveSceneFile` declaration (line ~24), add:

```cpp
// Serialize a scene to a JSON string (same schema as saveSceneFile, compact —
// no pretty-print, since this feeds the undo stack, not a human-diffable file).
std::string sceneToJsonString(const Reflection& reflection, const SceneFile& scene);

// Parse a scene from a JSON string. Returns nullopt on malformed JSON (logs via
// Log::error). Missing optional fields fall back to struct defaults — identical
// semantics to loadSceneFile, minus the file I/O.
std::optional<SceneFile> sceneFromJsonString(const Reflection& reflection,
                                             const std::string& json);
```

- [ ] **Step 2: Write the failing test**

In `tests/test_scene_io.cpp`, add a round-trip block inside `main()` (before the final `return`). The file has `using namespace iron;` (so types are unqualified) and a `makeReflectionRegistry()` helper that returns a fully-registered `Reflection`. Use these fields which are known to round-trip:

```cpp
    // In-memory string round-trip (M57): toJsonString -> fromJsonString
    // preserves entity name + transform + a material factor.
    {
        const Reflection r = makeReflectionRegistry();
        SceneFile s;
        SceneEntity e;
        e.name = "undo_probe";
        e.transform.position = {1.0f, 2.0f, 3.0f};
        e.material.metallic = 0.25f;
        s.entities.push_back(e);

        const std::string json = sceneToJsonString(r, s);
        CHECK(!json.empty());
        auto back = sceneFromJsonString(r, json);
        CHECK(back.has_value());
        CHECK(back->entities.size() == 1u);
        CHECK(back->entities[0].name == "undo_probe");
        CHECK_NEAR(back->entities[0].transform.position.x, 1.0f);
        CHECK_NEAR(back->entities[0].transform.position.z, 3.0f);
        CHECK_NEAR(back->entities[0].material.metallic, 0.25f);

        // Malformed input -> nullopt.
        CHECK(!sceneFromJsonString(r, "{ not json").has_value());
    }
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_scene_io --config Debug
```
Expected: FAIL — compile/link error: `sceneToJsonString` / `sceneFromJsonString` undeclared/unresolved.

- [ ] **Step 4: Refactor the file helpers and implement the string helpers**

In `engine/scene/SceneIO.cpp`, replace the body of `saveSceneFile` and `loadSceneFile` so the JSON build/parse lives in the new string functions. Replace the two functions (lines ~71-158) with:

```cpp
std::string sceneToJsonString(const Reflection& reflection, const SceneFile& scene) {
    json root = json::object();
    root["clearColor"] = toJson(scene.clearColor);

    json sun = json::object();
    sun["direction"] = toJson(scene.sun.direction);
    sun["color"]     = toJson(scene.sun.color);
    sun["ambient"]   = scene.sun.ambient;
    root["sun"] = sun;

    json fog = json::object();
    fog["color"]   = toJson(scene.fog.color);
    fog["density"] = scene.fog.density;
    root["fog"] = fog;

    json pls = json::array();
    for (const auto& pl : scene.pointLights) {
        json j = json::object();
        j["position"]  = toJson(pl.position);
        j["color"]     = toJson(pl.color);
        j["intensity"] = pl.intensity;
        j["range"]     = pl.range;
        pls.push_back(j);
    }
    root["pointLights"] = pls;

    json ents = json::array();
    for (const auto& e : scene.entities) ents.push_back(entityToJson(reflection, e));
    root["entities"] = ents;

    return root.dump();
}

std::optional<SceneFile> sceneFromJsonString(const Reflection& reflection,
                                             const std::string& jsonStr) {
    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const json::parse_error& e) {
        Log::error("SceneIO: parse error: %s", e.what());
        return std::nullopt;
    }

    SceneFile scene;
    readVec3(root, "clearColor", scene.clearColor);

    if (root.contains("sun")) {
        const json& sun = root["sun"];
        readVec3 (sun, "direction", scene.sun.direction);
        readVec3 (sun, "color",     scene.sun.color);
        readFloat(sun, "ambient",   scene.sun.ambient);
    }
    if (root.contains("fog")) {
        const json& fog = root["fog"];
        readVec3 (fog, "color",   scene.fog.color);
        readFloat(fog, "density", scene.fog.density);
    }
    if (root.contains("pointLights") && root["pointLights"].is_array()) {
        for (const auto& j : root["pointLights"]) {
            PointLight pl;
            readVec3 (j, "position",  pl.position);
            readVec3 (j, "color",     pl.color);
            readFloat(j, "intensity", pl.intensity);
            readFloat(j, "range",     pl.range);
            scene.pointLights.push_back(pl);
        }
    }
    if (root.contains("entities") && root["entities"].is_array()) {
        for (const auto& j : root["entities"]) {
            scene.entities.push_back(entityFromJson(reflection, j));
        }
    }
    return scene;
}

bool saveSceneFile(const Reflection& reflection,
                   const SceneFile& scene,
                   const std::string& path) {
    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    // Pretty-print the on-disk file (human-diffable); the string helper is compact.
    json root = json::parse(sceneToJsonString(reflection, scene));
    f << root.dump(2);
    return true;
}

std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return sceneFromJsonString(reflection, contents);
}
```

Add `#include <iterator>` and `#include <string>` near the existing includes at the top of `SceneIO.cpp` (the `<fstream>` include is already there; `<iterator>` is needed for `std::istreambuf_iterator`).

(The `saveSceneFile` round-trips through `json::parse` only to re-indent for the on-disk pretty-print; this keeps the schema defined in exactly one place. The extra parse is negligible — saving is a rare, user-initiated action.)

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_scene_io --config Debug && ctest --test-dir build-vk -C Debug -R test_scene_io --output-on-failure
```
Expected: PASS — `test_scene_io` passes including the new round-trip block.

- [ ] **Step 6: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/scene/SceneIO.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp && git commit -F tmp/commit-msg.txt
```
(Subject: `M57: in-memory sceneToJsonString / sceneFromJsonString (single serializer)`.)

---

### Task 3: Expose Node Editor focus from `NodeGraphPanel`

**Files:**
- Modify: `engine/editor/NodeGraphPanel.h`
- Modify: `engine/editor/NodeGraphPanel.cpp`

- [ ] **Step 1: Add a `focused()` accessor + member to the header**

In `engine/editor/NodeGraphPanel.h`, add a public accessor after `resetPlacement()` (line ~31) and a private member after `placed_` (line ~38):

```cpp
    // Whether the "Node Editor" window was focused on the last draw(). Used by
    // the host to route Ctrl+Z/Y to the graph history vs the scene history.
    bool focused() const { return focused_; }
```

and in the private section:

```cpp
    bool focused_ = false;            // ImGui::IsWindowFocused() at last draw
```

- [ ] **Step 2: Set the flag inside `draw()`**

In `engine/editor/NodeGraphPanel.cpp`, find the `ImGui::Begin("Node Editor" ...)` call that opens the panel window. Immediately after that `Begin(...)` (while the window is current, before the early-out `End()` paths), set:

```cpp
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
```

`RootAndChildWindows` is required because the node-editor canvas is a child window; plain `IsWindowFocused()` would read false while the user interacts with the canvas. If `draw()` has multiple `End()` return paths, setting the flag right after the opening `Begin()` covers all of them (the value reflects the window that was just begun).

- [ ] **Step 3: Build to verify it compiles**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target ironcore_editor --config Debug
```
Expected: builds clean (no new warnings).

- [ ] **Step 4: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/editor/NodeGraphPanel.h engine/editor/NodeGraphPanel.cpp && git commit -F tmp/commit-msg.txt
```
(Subject: `M57: NodeGraphPanel::focused() for Ctrl+Z routing`.)

---

### Task 4: Host integration in `main.cpp`

This task wires both documents into the editor: a `DocHistory` helper, `rebuildDerivedFromScene()`, baselines, clear-on-Play, Ctrl+Z/Y latching, focus-routed apply, and per-frame coalescing. It is verified at the visual gate (build + run), not by a unit test.

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Include the new header**

Near the other editor includes at the top of `games/11-sandbox/main.cpp` (where `SceneInspector.h` / `NodeGraphPanel.h` are included), add:

```cpp
#include "editor/UndoHistory.h"
```

- [ ] **Step 2: Add the `DocHistory` helper + a coalescing tick, after the node panel construction**

After `iron::NodeGraphPanel nodeGraphPanel;` (line ~777) and the graph seed block that ends with `graphModel.clearDirty();` (line ~798), add:

```cpp
    // --- M57: undo/redo ---------------------------------------------------
    // One snapshot stack per document (scene + node graph). `last` is the last
    // serialized state we observed; a transaction opens when the live state
    // first differs from `last`, and commits (one undo entry) when the document
    // is stable and the user isn't mid-interaction — so a whole drag = 1 entry.
    struct DocHistory {
        iron::UndoHistory hist{100};
        std::string last;            // last-seen serialized state
        std::string pendingBefore;   // snapshot captured when the txn opened
        bool        txnOpen = false;
    };
    DocHistory sceneDoc;
    DocHistory graphDoc;

    // Advance one document's coalescing state. `cur` = current serialized state;
    // `stable` = document settled AND no interaction in flight.
    auto tickDoc = [](DocHistory& d, const std::string& cur, bool stable) {
        if (cur != d.last) {
            if (!d.txnOpen) { d.pendingBefore = d.last; d.txnOpen = true; }
            d.last = cur;
        }
        if (d.txnOpen && stable) {
            d.hist.commit(d.pendingBefore);
            d.txnOpen = false;
        }
    };
```

- [ ] **Step 3: Add `rebuildDerivedFromScene()` next to `appendAndSelect`**

Immediately after the `appendAndSelect` lambda (ends at line ~964), add a lambda that tears down and rebuilds the derived mirrors from `scene.entities`, mirroring the startup resolve loop (lines 334-347). It captures `resolveEntity`, `toRenderHandles`, `world`, `resolved`, `sceneIndexToEntity`, `selectedIndex` (all in scope):

```cpp
    // M57: rebuild ALL derived state (World + resolved + index map) from the
    // current scene.entities. Used by undo/redo restore. Mirrors the startup
    // resolve loop exactly, including its skip-on-failed-resolve behavior.
    auto rebuildDerivedFromScene = [&]() {
        for (iron::EntityId e : sceneIndexToEntity) world.destroy(e);
        sceneIndexToEntity.clear();
        resolved.clear();
        for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {
            ResolvedEntity re;
            if (!resolveEntity(scene.entities[ei], ei, re)) continue;
            resolved.push_back(re);
            const iron::SceneEntity& se = scene.entities[ei];
            iron::EntityId entity = world.create();
            world.add<iron::Transform>(entity, se.transform);
            world.add<iron::MeshRef>(entity, se.mesh);
            world.add<iron::MaterialDef>(entity, se.material);
            world.add<iron::RenderHandles>(entity, toRenderHandles(re));
            sceneIndexToEntity.push_back(entity);
        }
        if (selectedIndex >= static_cast<int>(scene.entities.size()))
            selectedIndex = -1;
    };
```

- [ ] **Step 4: Initialize the baselines + clear-on-Play**

Two edits:

(a) Just before `app.setUpdate(...)` (line ~967), after all startup entities are resolved and the graph is seeded, seed both documents' `last` so the first real edit opens a transaction against the correct before-state:

```cpp
    // M57: baseline snapshots (after all startup entities + the seeded graph).
    sceneDoc.last = iron::sceneToJsonString(reflection, scene);
    graphDoc.last = graphModel.toJson().dump();
```

(b) In `togglePlayMode`, in the Edit→Play branch (after `editor.setMode(iron::EditorState::Mode::Play);`, line ~714) and the Play→Edit branch (after `scene = editScene;`, line ~702) — clear history when entering Play, and re-baseline both docs when returning to Edit (the restored scene/graph are a fresh starting point):

In the Play→Edit branch, after `cam = editCam;` (line ~704):
```cpp
            sceneDoc.hist.clear(); graphDoc.hist.clear();
            sceneDoc.txnOpen = false; graphDoc.txnOpen = false;
            sceneDoc.last = iron::sceneToJsonString(reflection, scene);
            graphDoc.last = graphModel.toJson().dump();
```

In the Edit→Play branch, after `editor.setMode(iron::EditorState::Mode::Play);` (line ~714):
```cpp
            sceneDoc.hist.clear(); graphDoc.hist.clear();
            sceneDoc.txnOpen = false; graphDoc.txnOpen = false;
```

(`togglePlayMode` is defined at line ~698, before `sceneDoc`/`graphDoc`/`rebuildDerivedFromScene` are declared. Since `togglePlayMode` is a `[&]` lambda only *invoked* later, move the `DocHistory sceneDoc; DocHistory graphDoc;` declarations and the `tickDoc` lambda from Step 2 to ABOVE `togglePlayMode` — i.e. declare them right after `graphModel`/`nodeGraphPanel`… but those are at line ~776, after `togglePlayMode` at ~698. Resolve the ordering by declaring `DocHistory sceneDoc; DocHistory graphDoc;` immediately before `togglePlayMode` (line ~697), and keep `tickDoc` / the baseline seeding where they are. `DocHistory` only needs `iron::UndoHistory` + `std::string`, so it can be declared early. Move the `struct DocHistory { … };` definition and the two instances to just before `auto togglePlayMode`.)

- [ ] **Step 5: Latch Ctrl+Z / Ctrl+Y in `setUpdate`**

First declare the latch flags next to the existing ones at line ~836:

```cpp
    bool wantUndo = false;
    bool wantRedo = false;
```

Then in `setUpdate`, inside the `if (!editor.isPlaying() && !look)` block (line ~1114), alongside the delete/duplicate latch but NOT gated on `viewport.focused` (undo works whether the viewport or the Node Editor is focused — routing happens at apply time), add:

```cpp
            // M57: latch undo/redo (Edit mode only). Suppressed while ImGui owns
            // the keyboard (typing in a field). Ctrl+Z = undo, Ctrl+Shift+Z /
            // Ctrl+Y = redo. Routing (scene vs graph) is decided in render() by
            // which panel is focused.
            if (!imgui.wantsKeyboard() && input.keyDown(GLFW_KEY_LEFT_CONTROL)) {
                if (input.keyPressed(GLFW_KEY_Z)) {
                    if (input.keyDown(GLFW_KEY_LEFT_SHIFT)) wantRedo = true;
                    else                                    wantUndo = true;
                } else if (input.keyPressed(GLFW_KEY_Y)) {
                    wantRedo = true;
                }
            }
```

- [ ] **Step 6: Capture the inspector-changed signal**

The Inspector draw currently ignores its return value (line ~1263). Capture it so the scene coalescing can use it as an "edit plausible" signal. Change:

```cpp
            inspector.draw(reflection,
                           inspValid ? &scene.entities[selectedIndex] : nullptr, sp, ek);
```
to:
```cpp
            inspectorChanged = inspector.draw(reflection,
                           inspValid ? &scene.entities[selectedIndex] : nullptr, sp, ek);
```

Declare `bool inspectorChanged = false;` at the top of `setRender`'s body (just after `imgui.beginFrame();`, line ~1189) so it resets each frame and is visible to the later coalescing block. Also declare a structural-edit signal there:

```cpp
        bool inspectorChanged   = false;
        bool structuralEdit     = false;   // an add/delete/duplicate ran this frame
```

- [ ] **Step 7: Set the structural-edit signal in the add/delete block**

In the `if (action == Action::...)` chain (lines 1394-1439), set `structuralEdit = true;` in each branch that mutates `scene.entities`. The simplest single edit: right after the chain, before the closing brace of the `if (!editor.isPlaying())` block at line ~1440, add:

```cpp
            if (action == Action::AddCube || action == Action::AddPlane ||
                action == Action::AddGltf || action == Action::Duplicate ||
                action == Action::Delete)
                structuralEdit = true;
```

- [ ] **Step 8: Apply undo/redo, then run the coalescing tick**

After the `if (!editor.isPlaying())` edit block closes (line ~1440) and BEFORE the "re-derive render data" loop (line ~1446), insert the apply + coalescing. Undo/redo and history tracking run only in Edit mode:

```cpp
        // --- M57: undo/redo apply + coalescing (Edit mode only) ---
        if (!editor.isPlaying()) {
            const bool nodeFocused = nodeGraphPanel.focused();

            // Apply a requested undo/redo to the focused document.
            if (wantUndo || wantRedo) {
                if (nodeFocused) {
                    const std::string cur = graphModel.toJson().dump();
                    auto restored = wantRedo ? graphDoc.hist.redo(cur)
                                             : graphDoc.hist.undo(cur);
                    if (restored) {
                        auto parsed = nlohmann::json::parse(*restored, nullptr, false);
                        if (!parsed.is_discarded() && graphModel.loadFromJson(parsed))
                            nodeGraphPanel.resetPlacement();
                        graphDoc.last    = graphModel.toJson().dump();
                        graphDoc.txnOpen = false;
                        graphModel.clearDirty();
                    }
                } else {
                    const std::string cur = iron::sceneToJsonString(reflection, scene);
                    auto restored = wantRedo ? sceneDoc.hist.redo(cur)
                                             : sceneDoc.hist.undo(cur);
                    if (restored) {
                        auto s = iron::sceneFromJsonString(reflection, *restored);
                        if (s) { scene = *s; rebuildDerivedFromScene(); }
                        sceneDoc.last    = iron::sceneToJsonString(reflection, scene);
                        sceneDoc.txnOpen = false;
                    }
                }
            }

            // Per-frame coalescing. Serialize the scene only when an edit is
            // plausible (keeps idle cost ~zero); the graph is cheap so we key it
            // off the model's dirty flag (set by every model mutation, incl.
            // node-position drags) or an already-open txn.
            const bool anyMouseDown = ImGui::IsAnyMouseDown();
            const bool sceneStable   = !anyMouseDown && !gizmo.dragging();
            const bool sceneSignal   = inspectorChanged || gizmo.dragging() ||
                                       structuralEdit || sceneDoc.txnOpen;
            if (sceneSignal) {
                const std::string cur = iron::sceneToJsonString(reflection, scene);
                tickDoc(sceneDoc, cur, sceneStable);
            }

            const bool graphSignal = graphModel.dirty() || graphDoc.txnOpen;
            if (graphSignal) {
                const std::string cur = graphModel.toJson().dump();
                tickDoc(graphDoc, cur, !anyMouseDown);
                graphModel.clearDirty();
            }
        }
        wantUndo = false;
        wantRedo = false;
```

Notes:
- The undo apply runs BEFORE the re-derive loop (line 1446) so the rebuilt `resolved` model matrices are then refreshed by it (harmless double-work, keeps the existing flow intact).
- `graphModel.clearDirty()` here means the model's dirty flag is "consumed" by the undo checkpoint each frame; nothing else in the host depends on it persisting across frames (it is only seeded at startup and read by tests).
- The node panel's Assign/Load-from-entity actions (lines 1286-1295) run earlier in the frame and mutate the graph; those changes are caught by the same `graphModel.dirty()` signal (Load) — Assign writes the entity's `logicGraph` string (a scene-side change to `scene.entities[selectedIndex]`), which the scene coalescing catches via the next plausible scene serialize. Assign is an explicit button press; if it should produce a scene undo entry, set `structuralEdit = true` when `act == Action::Assign`. Add that one line in the node-panel block:

```cpp
            if (selValid && act == iron::NodeGraphPanel::Action::Assign) {
                scene.entities[selectedIndex].logicGraph = graphModel.toJson().dump();
                structuralEdit = true;   // M57: Assign mutates the scene
            }
```

(`structuralEdit` is declared at the top of `setRender` in Step 6, before the node-panel block at line ~1286, so it is in scope there.)

- [ ] **Step 9: Build the sandbox**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Expected: builds clean. If `GLFW_KEY_Z` / `GLFW_KEY_Y` / `GLFW_KEY_LEFT_SHIFT` are not visible, confirm the GLFW header is already included (it is — the file uses `GLFW_KEY_DELETE` etc.).

- [ ] **Step 10: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add games/11-sandbox/main.cpp && git commit -F tmp/commit-msg.txt
```
(Subject: `M57: wire scene + graph undo/redo into the sandbox editor`.)

---

### Task 5: Full build, test sweep, and visual gate

**Files:** none (verification + memory).

- [ ] **Step 1: Clean full build of all targets**

Per [[verify-clean-build-before-ci]], build everything (not incremental targets) so a stale binary can't hide an interface break:

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --config Debug
```
Expected: all targets build with no errors.

- [ ] **Step 2: Run the full test suite**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: every test passes, including `test_undo_history` and `test_scene_io`. Record the N/N count for the PR body. Do NOT proceed to a commit/PR if any test fails.

- [ ] **Step 3: Code review**

Per [[always-code-review-changes]], run a best-practices review on the full diff (`git diff main...HEAD`) before the visual gate — focus on the coalescing logic (one-gesture-one-entry correctness), the rebuild parity with startup, and lifetime/capture correctness of the new lambdas.

- [ ] **Step 4: Hand off to the user's visual gate**

Launch the sandbox and verify by hand (this is a UI milestone — the wiring is visual-gated):
- Scene: move an entity with the gizmo, then Ctrl+Z → it returns to the prior transform; Ctrl+Y → re-applies. A single slow drag is exactly ONE undo step (not zero, not many).
- Scene: add a cube, Ctrl+Z removes it; delete an entity, Ctrl+Z restores it (and its render/collider/probe visuals); duplicate, Ctrl+Z removes the copy.
- Scene: edit an Inspector field (e.g. a material slider), Ctrl+Z reverts it.
- Graph: with the Node Editor focused, move a node / add a node / delete a node / connect two pins, then Ctrl+Z undoes that graph edit and Ctrl+Y redoes it — and these do NOT affect the scene selection.
- Routing: Ctrl+Z while the Node Editor is focused touches only the graph; while the viewport is focused, only the scene.
- Press Play → both histories clear (Ctrl+Z does nothing in Play); Stop → back to a clean Edit baseline.

- [ ] **Step 5: Update memory after the gate passes**

Update `memory/iron-core-engine-progress.md` (append an M57 entry) and the `MEMORY.md` LATEST marker. Mark the [[editor-undo-redo-milestone]] memory as delivered (or update it to point at the implementation). Note the design doc + plan paths.

---

## Self-Review

**Spec coverage:**
- Generic `UndoHistory` instantiated per document → Task 1 + Task 4 (two `DocHistory`).
- Scene serialize/restore via in-memory helpers + mirror rebuild → Task 2 (`sceneToJsonString`/`sceneFromJsonString`) + Task 4 (`rebuildDerivedFromScene`).
- Graph serialize/restore via existing `toJson()`/`loadFromJson()` + `resetPlacement()` → Task 4.
- One-gesture-one-entry coalescing (txn opens on first diff, commits when stable) → Task 4 Step 2 + Step 8.
- Routing by focus → Task 3 (`focused()`) + Task 4 Step 8.
- Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, suppressed in text fields → Task 4 Step 5.
- Cleared on Play, survives Save → Task 4 Step 4 (Save is untouched, so it inherently survives).
- Capacity cap, empty-stack no-op, malformed-snapshot fail-safe → Task 1 (cap + no-op) + Task 2 (`sceneFromJsonString` returns nullopt; apply guards with `if (s)` / `is_discarded()`).
- Unit tests for the stack → Task 1; scene round-trip → Task 2; wiring visual-gated → Task 5.

**Placeholder scan:** No "TBD"/"add error handling" placeholders — every code step shows complete code. Error handling is concrete (nullopt guards, `is_discarded()`).

**Type consistency:** `UndoHistory::commit/undo/redo/canUndo/canRedo/clear` match between header (Task 1 Step 1), test (Step 2), and impl (Step 5). `DocHistory{hist,last,pendingBefore,txnOpen}` fields are consistent across Steps 2/4/8. `sceneToJsonString`/`sceneFromJsonString` signatures match between header (Task 2 Step 1), impl (Step 4), and call sites (Task 4 Steps 4/8). `nodeGraphPanel.focused()` matches between Task 3 and Task 4 Step 8. `rebuildDerivedFromScene` / `tickDoc` / `inspectorChanged` / `structuralEdit` / `wantUndo` / `wantRedo` are each defined once and used consistently.

**Known fragility carried over (not introduced):** the startup resolve loop skips entities that fail to resolve, which can desync `sceneIndexToEntity` from `scene.entities`; `rebuildDerivedFromScene` replicates this exactly, so behavior is unchanged. All demo entities resolve, so the parallel-index invariant holds in practice.
