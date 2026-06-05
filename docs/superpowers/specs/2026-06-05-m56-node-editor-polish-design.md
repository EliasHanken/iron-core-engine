# M56 — Node Editor Polish — Design

> **Polish milestone on the node-system track** (after M53 core, M54 editor, M55 gameplay logic). Makes the Node Editor genuinely usable — the editor the user will live in for the remaining node milestones (#4 shaders, #5 VFX, #6 UI). Driven by visual-gate feedback: the nodes are functional but hard to read (all pins render in one left column, no color), the panel floats, selecting a scene object yanks focus to the Inspector, and dragged node positions don't persist.

## Goal

Give the Node Editor the **imgui-node-editor "blueprint" look** (category-tinted node headers, typed/colored icon pins, inputs-left / outputs-right) and fix three UX papercuts: dock the panel, stop the select→Inspector focus jump, and persist node positions to the graph JSON.

## Scope (locked in brainstorming)

Full blueprint-example fidelity for the node styling, plus the three fixes. No node search/context-add, comment/group nodes, minimap, reroute nodes, per-instance colors, or theme config (all out of scope).

## Architecture

Reuse thedmd's **blueprint-example rendering utilities** (part of the imgui-node-editor library already vendored in M54): the `BlueprintNodeBuilder` (header + input/middle/output layout) and `ax::Widgets::Icon` (typed pin icons). The `NodeGraphPanel`'s node-draw loop is rewritten on top of them. The headless `GraphEditorModel` and the M55 runtime are untouched — this is purely the panel's rendering + three host/panel UX fixes.

## Components

### 1. Vendor the blueprint utilities
Copy these from imgui-node-editor's `examples/blueprints-example/utilities/` into `third_party/imgui-node-editor/utilities/`:
- `builders.h` / `builders.cpp` — `util::BlueprintNodeBuilder` (`Begin(nodeId)`, `Header(color)`, `EndHeader()`, `Input(pinId)`/`EndInput()`, `Middle()`, `Output(pinId)`/`EndOutput()`, `End()`).
- `widgets.h` / `widgets.cpp` (and `drawing.h`/`drawing.cpp` if the version splits the icon drawing out) — `ax::Widgets::Icon(size, IconType, filled, color, innerColor)` + `enum class IconType { Flow, Circle, Square, Grid, RoundSquare, Diamond }`.

Add the new `.cpp` files to the existing `third_party/imgui-node-editor/CMakeLists.txt` `imgui_node_editor` target (they compile against the same ImGui + node-editor headers). Obtain them by cloning thedmd/imgui-node-editor (as in M54) and copying the actual files from `examples/blueprints-example/utilities/` — use what the clone provides (file set is stable but verify). Keep the upstream license headers. `BlueprintNodeBuilder` needs a header-background texture handle; the upstream builder accepts `0`/no texture and falls back to a solid header fill — use that (no texture asset needed).

### 2. Rewrite `NodeGraphPanel`'s node draw
Replace the current single-loop pin draw with a `BlueprintNodeBuilder` layout per node:
- `builder.Begin(nodeId)`.
- `builder.Header(categoryColor(typeName's category)); ImGui::TextUnformatted(typeName); builder.EndHeader();`
- For each **input** port (in registration order): `builder.Input(pinId)`, draw `Widgets::Icon` (Flow arrow for `Exec`, Circle for data) colored by `pinColor(type)` and filled iff the pin is connected, then the label, then (for an unconnected data input) the inline literal widget; `builder.EndInput()`.
- `builder.Middle();` (optional spacer).
- For each **output** port: `builder.Output(pinId)`, the label then the `Widgets::Icon`; `builder.EndOutput()`.
- `builder.End();`
- **`pinColor(PortType)`**: Exec=white `(1,1,1)`, Bool=red `(0.86,0.2,0.2)`, Int=cyan `(0.27,0.79,0.86)`, Float=green `(0.55,0.86,0.2)`, Vec2/Vec3/Vec4=gold `(0.96,0.79,0.0)`, String=pink `(0.86,0.2,0.6)`.
- **`categoryColor(category)`**: Event=red `(0.5,0.15,0.15)`, Flow=slate `(0.2,0.2,0.3)`, Math=green `(0.15,0.4,0.2)`, Transform=orange `(0.45,0.3,0.1)`, Variable=purple `(0.35,0.15,0.45)`, Value=teal `(0.1,0.35,0.4)`, Sink=grey `(0.3,0.3,0.3)`, default grey. (Header colors are dark tints; ImColor.)
- Links keep the existing id mapping; optionally tint a link by its source pin's type (nice-to-have, not required).
- The pin id / link id encoding, create/delete handling, palette, toolbar, and outputs readout are unchanged.

### 3. Dock the panel
In the host's one-time DockBuilder layout (`games/11-sandbox/main.cpp`, where `DockBuilderDockWindow("Viewport"/"Scene Outliner"/…)` runs), add the Node Editor to a **bottom split** under the Viewport: split the center/viewport node downward (`DockBuilderSplitNode(center, ImGuiDir_Down, 0.35f, &bottom, &center)`) and `DockBuilderDockWindow("Node Editor", bottom)`. (Adjust the existing split sequence so the Viewport keeps the upper area.)

### 4. Fix the select→Inspector focus jump
Investigate why selecting a scene entity pulls focus to the Inspector tab (likely the Inspector or Outliner calls `ImGui::SetWindowFocus`, or a tab-bar auto-select on a docked group). Remove/avoid the focus grab so selecting an object keeps the user's current panel/tab active. The fix is whatever the investigation reveals — most likely deleting a stray `SetWindowFocus("Inspector")` or equivalent; if it's structural (panels share a tab group), dock them so they don't fight. Document the cause in the commit.

### 5. Persist node positions
Before serializing (Save / Assign-to-entity), sync each node's live canvas position back into the model so the JSON carries it:
- Add `GraphEditorModel::setNodePosition(NodeId, float x, float y)` (sets `editorX/editorY`, dirties) — headless, unit-testable.
- In the panel, just before producing JSON for Save/Assign (or each frame after the canvas), read `ed::GetNodePosition(nodeId)` per node and call `model.setNodePosition(...)`. On load, nodes are placed from `editorX/editorY` (already wired via the `placed_` set). So drag a node, Save, Load → it returns to where you left it.

## Data flow

Unchanged from M54/M55 except rendering: `model.graph()` → per node, `BlueprintNodeBuilder` draws header + typed-icon input/output columns → canvas interactions drive the model as before. Positions sync model↔`ed::` so they round-trip through JSON.

## Error handling

- Missing/odd `category` → default grey header. Unknown `PortType` → default white pin. (No crash.)
- Vendored utility compile issues against the project's ImGui version (as in M54, the lib predates ImGui 1.92) → resolve with the same shim pattern; report if a utility file needs guarding.

## Testing

Almost entirely visual → **visual-gated**. The one unit-testable seam:
- **`tests/test_graph_editor.cpp`** gains a node-position round-trip: `setNodePosition` updates `editorX/editorY` + dirties; `toJson`→`loadFromJson` preserves positions (a graph saved with a moved node reloads at that position).

The blueprint look (headers, pin icons/colors, two-column layout), docking, and the focus fix are confirmed at the **visual gate** (open the editor, load the bob graph, see colored typed pins with inputs left / outputs right and a tinted header; the panel is docked; selecting a scene object no longer jumps to Inspector; move a node, Save+Load, it stays put).

## Files

**New:** `third_party/imgui-node-editor/utilities/builders.{h,cpp}`, `widgets.{h,cpp}` (+ `drawing.{h,cpp}` if present).
**Modified:** `third_party/imgui-node-editor/CMakeLists.txt` (add the utility sources), `engine/editor/NodeGraphPanel.{h,cpp}` (blueprint node draw + position sync), `engine/nodes/GraphEditorModel.{h,cpp}` (`setNodePosition`), `games/11-sandbox/main.cpp` (dock the panel + the focus fix + per-frame position sync if done host-side), `tests/test_graph_editor.cpp` (position round-trip test).

## Out of scope (→ later)

Node search / context-menu add (Blueprints' right-click search), comment & group nodes, minimap/navigation, reroute nodes, per-instance node colors, link-type tinting (optional nice-to-have), a theme/config system, and any change to the graph model/runtime semantics.
