# M56 — Node Editor Polish — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Node Editor the imgui-node-editor "blueprint" look (category-tinted headers, typed/colored icon pins, inputs-left / outputs-right) and fix three UX papercuts: dock the panel, stop the select→Inspector focus jump, and persist node positions to JSON.

**Architecture:** Reuse thedmd's blueprint-example rendering utilities (`BlueprintNodeBuilder` + `ax::Widgets::Icon`) from the already-vendored imgui-node-editor library; rewrite `NodeGraphPanel`'s node-draw on top of them. The headless `GraphEditorModel` gains `setNodePosition` (the one unit-tested seam). The model + M55 runtime semantics are untouched.

**Tech Stack:** C++17, ImGui + imgui-node-editor (`ed::`, vendored in M54), the blueprint utilities (to vendor), `engine/nodes/` + `engine/editor/`, the `test_framework.h` harness. Canonical build dir `build-vk`; ctest `-C Debug`.

**Spec:** `docs/superpowers/specs/2026-06-05-m56-node-editor-polish-design.md`

**Conventions:** end every commit body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push (PR at end). Branch `m56-node-editor-polish` (off merged `main`).

**Risk note:** Task 1 is headless TDD (full code). Tasks 2–5 are vendored-utility integration + an ImGui rewrite + UX investigation — concrete guidance + the `BlueprintNodeBuilder`/`Icon` API, but the implementer binds exact signatures against the vendored headers and validates at the **visual gate**.

---

## File Structure

- **`engine/nodes/GraphEditorModel.{h,cpp}`** (modify) — add `setNodePosition`.
- **`tests/test_graph_editor.cpp`** (modify) — position round-trip test.
- **`third_party/imgui-node-editor/utilities/`** (new) — vendored `builders.{h,cpp}`, `widgets.{h,cpp}` (+ `drawing.{h,cpp}` if split).
- **`third_party/imgui-node-editor/CMakeLists.txt`** (modify) — add the utility sources.
- **`engine/editor/NodeGraphPanel.{h,cpp}`** (modify) — blueprint node draw + per-frame position sync.
- **`games/11-sandbox/main.cpp`** (modify) — dock the panel + the focus fix.

---

## Task 1: GraphEditorModel::setNodePosition + position round-trip (headless TDD)

**Files:** Modify `engine/nodes/GraphEditorModel.h/.cpp`; test `tests/test_graph_editor.cpp`.

- [ ] **Step 1: Declare `setNodePosition` in `engine/nodes/GraphEditorModel.h`** (public, after `setLiteral`)

```cpp
    // Update a node's editor canvas position (persisted in toJson). Dirties.
    void setNodePosition(NodeId id, float x, float y);
```

- [ ] **Step 2: Add a failing test to `tests/test_graph_editor.cpp`** (before `return iron_test_result();`)

```cpp
    // setNodePosition updates editorX/Y + dirties; positions round-trip JSON.
    {
        GraphEditorModel m(&reg);
        const NodeId n = m.addNode("Const", 0, 0);
        m.clearDirty();
        m.setNodePosition(n, 123.0f, 45.0f);
        CHECK(m.dirty());
        CHECK_NEAR(m.graph().node(n)->editorX, 123.0f);
        CHECK_NEAR(m.graph().node(n)->editorY, 45.0f);

        const nlohmann::json j = m.toJson();
        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(j));
        CHECK_NEAR(m2.graph().node(n)->editorX, 123.0f);
        CHECK_NEAR(m2.graph().node(n)->editorY, 45.0f);
    }
```
(`reg` is the builtin `NodeRegistry` constructed at the top of `test_graph_editor.cpp`'s `main`. The node id `n` is 1 here so it survives the JSON round-trip — `fromJson` preserves ids.)

- [ ] **Step 3: Build to confirm RED**

Run: `cmake --build build-vk --config Debug --target test_graph_editor`
Expected: FAIL — `setNodePosition` undefined.

- [ ] **Step 4: Implement in `engine/nodes/GraphEditorModel.cpp`** (near `setLiteral`)

```cpp
void GraphEditorModel::setNodePosition(NodeId id, float x, float y) {
    if (Node* n = graph_.node(id)) {
        n->editorX = x;
        n->editorY = y;
        dirty_ = true;
    }
}
```
(`Graph::node(NodeId)` non-const returns `Node*` — confirm it exists; it does, used by `addNode`.)

- [ ] **Step 5: Build + run**

Run: `cmake --build build-vk --config Debug --target test_graph_editor && ctest --test-dir build-vk -C Debug -R test_graph_editor --output-on-failure`
Expected: PASS. (Confirm PASS before committing.)

- [ ] **Step 6: Commit**

```bash
git add engine/nodes/GraphEditorModel.h engine/nodes/GraphEditorModel.cpp tests/test_graph_editor.cpp
git commit -m "M56: GraphEditorModel::setNodePosition (canvas position persists in JSON)"
```

---

## Task 2: Vendor the blueprint utilities

**Files:** Create `third_party/imgui-node-editor/utilities/*`; modify `third_party/imgui-node-editor/CMakeLists.txt`. Integration; validation = it compiles into `imgui_node_editor`.

- [ ] **Step 1: Fetch the utilities**

Clone imgui-node-editor (as in M54) to a scratch dir and copy the blueprint utilities:
`git clone --depth 1 https://github.com/thedmd/imgui-node-editor tmp/ine-src` (tmp/ is gitignored). From `tmp/ine-src/examples/blueprints-example/utilities/`, copy into `third_party/imgui-node-editor/utilities/`:
`builders.h`, `builders.cpp`, `widgets.h`, `widgets.cpp`, and `drawing.h`, `drawing.cpp` if they exist in that folder (some versions inline drawing into widgets — use what the clone provides). Verify the actual file list; do NOT assume. Keep upstream license/notice if present. Remove the scratch clone after (`rm -rf tmp/ine-src`); do NOT git-add tmp/.

- [ ] **Step 2: Confirm the API by reading the copied headers**

Open `third_party/imgui-node-editor/utilities/builders.h` and `widgets.h`. Confirm:
- `util::BlueprintNodeBuilder` with `BlueprintNodeBuilder(ImTextureID texture = 0, int textureWidth = 0, int textureHeight = 0)`, `Begin(ed::NodeId)`, `Header(const ImVec4& color = ImVec4(1,1,1,1))`, `EndHeader()`, `Input(ed::PinId)`, `EndInput()`, `Middle()`, `Output(ed::PinId)`, `EndOutput()`, `End()`.
- `ax::Widgets::Icon(const ImVec2& size, IconType type, bool filled, const ImVec4& color = ImVec4(1,1,1,1), const ImVec4& innerColor = ImVec4(0,0,0,0))` and `enum class IconType { Flow, Circle, Square, Grid, RoundSquare, Diamond }` (namespace may be `ax::Drawing::IconType` in some versions — note it).
Adapt Task 3's calls to the exact names found.

- [ ] **Step 3: Add the sources to `third_party/imgui-node-editor/CMakeLists.txt`**

Add the new `.cpp` files to the `imgui_node_editor` STATIC library source list (alongside `imgui_node_editor.cpp` etc.):
```cmake
  utilities/builders.cpp
  utilities/widgets.cpp
```
(Add `utilities/drawing.cpp` too if it exists.) The `target_include_directories(... PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})` already covers the `utilities/` headers via `#include "utilities/builders.h"`; confirm the include path works (the utilities include `imgui_node_editor.h` + `imgui.h`, both already on the target).

- [ ] **Step 4: Smoke-compile**

Run: `cmake --build build-vk --config Debug --target imgui_node_editor`
Expected: the utilities compile into the lib. If they hit ImGui 1.92 compat issues (as the core lib did in M54), apply the same minimal shim and report it. If a utility references an example-only header (e.g. an `application` header), it's the wrong file — only `builders`/`widgets`/`drawing` are needed; drop any stray include.

- [ ] **Step 5: Commit**

```bash
git add third_party/imgui-node-editor/utilities third_party/imgui-node-editor/CMakeLists.txt
git commit -m "M56: vendor imgui-node-editor blueprint utilities (BlueprintNodeBuilder + Icon)"
```

---

## Task 3: Blueprint-style node rendering

**Files:** Modify `engine/editor/NodeGraphPanel.cpp` (and `.h` if helpers are added). Visual-gated.

> Read the current node-draw loop in `NodeGraphPanel.cpp` (the `for (const Node& n : model.graph().nodes())` block with `ed::BeginNode`/`EndNode`) and the vendored `builders.h`/`widgets.h` first.

- [ ] **Step 1: Add color helpers** (file-local, anonymous namespace in NodeGraphPanel.cpp)

```cpp
#include "utilities/builders.h"
#include "utilities/widgets.h"

namespace ed = ax::NodeEditor;
namespace util = ax::NodeEditor::Utilities;   // confirm the namespace from builders.h

namespace {

ImColor pinColor(PortType t) {
    switch (t) {
        case PortType::Exec:   return ImColor(255, 255, 255);
        case PortType::Bool:   return ImColor(220,  48,  48);
        case PortType::Int:    return ImColor( 68, 201, 156);
        case PortType::Float:  return ImColor(147, 226,  74);
        case PortType::Vec2:
        case PortType::Vec3:
        case PortType::Vec4:   return ImColor(245, 201,   0);
        case PortType::String: return ImColor(218,  60, 156);
    }
    return ImColor(255, 255, 255);
}

ImColor headerColor(const std::string& category) {
    if (category == "Event")     return ImColor(128,  38,  38);
    if (category == "Flow")      return ImColor( 51,  51,  77);
    if (category == "Math")      return ImColor( 38, 102,  51);
    if (category == "Transform") return ImColor(115,  77,  26);
    if (category == "Variable")  return ImColor( 89,  38, 115);
    if (category == "Value")     return ImColor( 26,  89, 102);
    if (category == "Sink")      return ImColor( 77,  77,  77);
    return ImColor( 64,  64,  64);
}

// Exec pins use the Flow (arrow) icon; data pins use a Circle.
ax::Widgets::IconType iconFor(PortType t) {
    return t == PortType::Exec ? ax::Widgets::IconType::Flow
                               : ax::Widgets::IconType::Circle;
}

}  // namespace
```
(Adapt `util::`/`ax::Widgets::IconType` namespaces to the vendored headers — Step 2 of Task 2 told you the real names.)

- [ ] **Step 2: Replace the node-draw loop body with the builder**

Replace the existing `ed::BeginNode(...) … ed::EndNode()` body (keep the outer `for` over `model.graph().nodes()` + the `const NodeTypeDesc* t = ...; if (!t) continue;` and the first-appearance `ed::SetNodePosition` placement) with a `BlueprintNodeBuilder` layout:

```cpp
        static util::BlueprintNodeBuilder builder;   // reused across nodes/frames
        builder.Begin(ed::NodeId(static_cast<std::uintptr_t>(n.id)));

        builder.Header(headerColor(t->category));
        ImGui::Spring(0);
        ImGui::TextUnformatted(t->typeName.c_str());
        ImGui::Spring(1);
        ImGui::Dummy(ImVec2(0, 28));
        builder.EndHeader();

        // Inputs (left column).
        for (int i = 0; i < (int)t->ports.size(); ++i) {
            const PortDesc& p = t->ports[i];
            if (p.dir != PortDir::In) continue;
            builder.Input(ed::PinId(pinId(n.id, i, false)));
            const bool connected = model.graph().incoming(n.id, p.name).has_value();
            ax::Widgets::Icon(ImVec2(20, 20), iconFor(p.type), connected,
                              pinColor(p.type), ImColor(32, 32, 32));
            ImGui::Spring(0);
            ImGui::TextUnformatted(p.name.c_str());
            // Inline literal for an unconnected data input (same widgets as before).
            if (p.type != PortType::Exec && !connected) {
                ImGui::Spring(0);
                /* PASTE the existing DragFloat/Checkbox/InputText literal block here,
                   unchanged, using model.setLiteral(...) */
            }
            ImGui::Spring(0);
            builder.EndInput();
        }

        builder.Middle();

        // Outputs (right column).
        for (int i = 0; i < (int)t->ports.size(); ++i) {
            const PortDesc& p = t->ports[i];
            if (p.dir != PortDir::Out) continue;
            builder.Output(ed::PinId(pinId(n.id, i, true)));
            ImGui::Spring(0);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::Spring(0);
            ax::Widgets::Icon(ImVec2(20, 20), iconFor(p.type),
                              /*outputs draw filled when they have any link; simplest: */ true,
                              pinColor(p.type), ImColor(32, 32, 32));
            builder.EndOutput();
        }

        builder.End();
```
> `ImGui::Spring`/`ImGui::Dummy` come from the imgui-node-editor `imgui_canvas`/extra widgets the builder relies on — `Spring` is declared by the library's `imgui_node_editor` extras; if `Spring` is unresolved, it's `ImGui::Spring` from the vendored `imgui_extra_math`/builder include — confirm and include accordingly (the blueprint example uses `ax::NodeEditor::Utilities` + `ImGui::Spring` from `imgui_node_editor.h`'s suspend/extra; mirror the example's includes). Keep the inline-literal widget block byte-for-byte from the current code — only its placement moves inside `builder.Input`. The pin id encoding (`pinId(n.id, i, isOut)`) is unchanged so links still resolve. Keep the existing link draw + create/delete handling AFTER the node loop unchanged.

- [ ] **Step 3: Build + visual smoke**

Run: `cmake --build build-vk --config Debug --target sandbox`
Expected: compiles + links. Resolve any builder/Icon/Spring signature mismatches against the vendored headers + the upstream `blueprints-example.cpp` (mirror how it calls `builder.Header`/`Input`/`Icon`/`Spring`) until it builds. The actual look is checked at the visual gate.

- [ ] **Step 4: Commit**

```bash
git add engine/editor/NodeGraphPanel.cpp engine/editor/NodeGraphPanel.h
git commit -m "M56: blueprint-style node rendering (header tint, typed icon pins, two-column)"
```

---

## Task 4: Dock the panel + persist positions

**Files:** Modify `games/11-sandbox/main.cpp`, `engine/editor/NodeGraphPanel.cpp`. Visual-gated.

- [ ] **Step 1: Dock the Node Editor into the layout**

In the one-time DockBuilder block (`games/11-sandbox/main.cpp`, where `DockBuilderDockWindow("Viewport", center)` etc. run — read it first), split the center node downward and dock the Node Editor there:
```cpp
                ImGuiID bottom;
                ImGuiID upper = ImGui::DockBuilderSplitNode(
                    center, ImGuiDir_Down, 0.35f, &bottom, &center);
                (void)upper;
                ImGui::DockBuilderDockWindow("Node Editor", bottom);
```
Place this with the other `DockBuilderDockWindow` calls (after the Viewport is assigned to `center`, before `DockBuilderFinish`). Adjust the split direction/ratio if needed so the Viewport keeps the upper area. (Confirm the variable names `center`/the dockspace id from the existing code.)

- [ ] **Step 2: Per-frame position sync in the panel**

In `NodeGraphPanel::draw`, AFTER `ed::Begin("canvas")` ... the node loop, BEFORE `ed::End()` (while the editor is current), write each node's live canvas position back to the model so it persists:
```cpp
    for (const Node& n : model.graph().nodes()) {
        const ImVec2 pos = ed::GetNodePosition(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        if (pos.x != n.editorX || pos.y != n.editorY)
            model.setNodePosition(n.id, pos.x, pos.y);
    }
```
> `ed::GetNodePosition` returns the node's canvas position; syncing only on change avoids needless dirtying. This makes Save/Assign capture the laid-out positions (which `loadFromJson` + the `placed_` placement restore). Confirm `ed::GetNodePosition`'s signature in `imgui_node_editor.h`.

- [ ] **Step 3: Build + full sweep**

Run: `cmake --build build-vk --config Debug --target sandbox` then `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: links + all green.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp engine/editor/NodeGraphPanel.cpp
git commit -m "M56: dock the Node Editor panel + persist node positions to JSON"
```

---

## Task 5: Fix the select→Inspector focus jump

**Files:** Modify whatever the investigation points to (likely `games/11-sandbox/main.cpp` or `engine/editor/SceneInspector.cpp`/`SceneOutliner.cpp`). Visual-gated.

- [ ] **Step 1: Find the cause**

Grep for forced focus / tab activation tied to selection:
`grep -rn "SetWindowFocus\|SetNextWindowFocus\|SetKeyboardFocusHere\|FocusWindow\|ImGuiTabItemFlags_SetSelected\|SetTabItemClosed" engine/editor/ games/11-sandbox/main.cpp`
Also check whether the Inspector window is `Begin`-ed with a flag forcing focus, or whether selecting in the SceneOutliner calls something that focuses the Inspector, or whether the Inspector and another panel share a dock tab so a content change brings it forward.

- [ ] **Step 2: Remove/avoid the focus grab**

Apply the minimal fix the cause dictates:
- If a `SetWindowFocus("Inspector")`/equivalent fires on selection change → delete it (selection shouldn't steal focus).
- If the Inspector forces focus via a window flag → drop the flag.
- If it's a shared-tab artifact → in Task 4's DockBuilder layout, ensure the Inspector and the panel the user works in aren't auto-focused on content change.
Goal: selecting a scene object in the Outliner/viewport leaves the user's current panel/tab active (the Node Editor stays focused if they were in it). Document the exact cause + fix in the commit message.

- [ ] **Step 3: Build + sweep**

Run: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: links + green (this is a UI behavior change; tests are unaffected).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "M56: stop selection from stealing focus to the Inspector tab"
```
(Stage only the files the fix actually touched — replace `-A` with the explicit paths.)

---

## Task 6: Update progress memory

- [ ] After the PR is opened, append an M56 entry to `iron-core-engine-progress.md` (blueprint-style node editor, the vendored utilities, the dock/focus/position fixes, PR number) + refresh the `MEMORY.md` index line. Documentation only.

---

## Visual Gate (after Task 5, with the user)

Open the sandbox editor (`sandbox.exe`). The **Node Editor is docked** (bottom split, not floating). Select entity 0 → **Load from entity** → the bob graph renders **blueprint-style**: each node has a **category-tinted header** with its name, **input pins down the left** and **output pins down the right**, each pin a **colored icon by type** (white arrow for exec, green circle for Float, gold for Vec3, etc.) — it's obvious where wires connect. Selecting a scene object **no longer jumps to the Inspector**. Drag a node, **Save** then **Load** → it stays where you put it. Iterate colors/sizing at the gate.

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- `setNodePosition` + position round-trip → Task 1. ✓
- Vendor blueprint utilities → Task 2. ✓
- Blueprint node draw (header tint, typed icon pins, two-column) → Task 3. ✓
- Dock the panel + persist positions → Task 4. ✓
- Fix the select→Inspector focus jump → Task 5. ✓
- Testing: position round-trip unit-tested (Task 1); the rest visual-gated. ✓
- Out-of-scope (search, comments, minimap, reroute, theme) not implemented. ✓

**2. Placeholder scan:** Task 1 is full code. Tasks 2–5 are third-party/UI/UX integration: they give concrete structure + the `BlueprintNodeBuilder`/`Icon`/`ed::GetNodePosition`/`DockBuilderSplitNode` API but require the implementer to bind exact signatures against the vendored headers + the upstream `blueprints-example.cpp` and to investigate the focus cause — the only honest way to do a vendored-UI-lib rewrite + a UX-bug fix whose cause isn't yet known. Flagged in the Risk note.

**3. Type consistency:** `GraphEditorModel::setNodePosition(NodeId,float,float)` (Task 1) is called by Task 4's sync. `pinColor`/`headerColor`/`iconFor` helpers (Task 3) used in the node loop. `pinId(n.id,i,isOut)` encoding unchanged from M54 (links still resolve). `ed::NodeId`/`ed::PinId`, `ed::GetNodePosition`, `ed::SetNodePosition`, `DockBuilderSplitNode`/`DockBuilderDockWindow`, `util::BlueprintNodeBuilder`, `ax::Widgets::Icon`/`IconType` reference the vendored library; `PortType`/`PortDir`/`PortDesc`/`NodeTypeDesc::{category,ports}` from the merged M53 headers. ✓
