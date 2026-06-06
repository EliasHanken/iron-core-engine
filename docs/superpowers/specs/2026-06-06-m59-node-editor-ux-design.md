# M59 — Node Editor UX v2 (gradient header + context menus) — Design

> **Node-system track**, after M53 core → M54 panel → M55 gameplay nodes → M56 polish v1 → M57 undo → M58 look v2 + groups. Driven by user feedback at the M58 gate: the node look still isn't the exact imgui-node-editor UE4-blueprint look, and the editor needs graph-building interactions (drag-from-pin to create, right-click menus) instead of the "Add" toolbar palette.

## Goal

Make the node editor look and feel like the imgui-node-editor blueprint demo: **(1)** an exact UE4-style **gradient header** (per-category tint), **(2)** **drag-from-a-pin onto empty canvas → a menu of type-compatible nodes that auto-wire**, and **(3)** **right-click context menus** for adding/deleting nodes, links, and comments — replacing the cramped "Add: …" toolbar palette.

## Scope (locked in brainstorming)

In: gradient header texture; drag-from-pin create menu (type-compatible, auto-wired); right-click background/node/pin/link menus; remove the Add toolbar palette (move add-node + add-comment into the menus). Out (YAGNI / later): fuzzy-search box in the create menu (categorized submenus only), node renaming, reroute nodes, copy/paste, multi-select operations. Also out: vendoring the real `BlueprintNodeBuilder` — it needs ImGui's `BeginVertical`/`Spring` stack-layout API which is **not** in ImGui 1.92.8 (confirmed); the gradient-texture approach achieves the same header look without that un-vendorable dependency. The zoomed-text blur is **accepted** (inherent to the node-editor canvas scaling; crisp at 100%).

## Architecture

All changes live in the node-editor layer; **no new third-party vendoring**. The vendored node-editor already exposes everything needed: `QueryNewNode`/`AcceptNewItem` (drag-to-empty), `Suspend`/`Resume` (open ImGui popups over the canvas), and `ShowBackgroundContextMenu`/`ShowNodeContextMenu`/`ShowPinContextMenu`/`ShowLinkContextMenu`. The look uses ImGui's `ImDrawList::AddImageRounded` (available).

Three units:
- **`GraphEditorModel` (headless):** a `compatibleCreations(PortType, PortDir)` query returning the node types creatable from a given pin + the target port to auto-wire — reusing the existing `dataCompatible()` / connect-validation rules. Unit-tested.
- **`ImGuiLayer`:** a small helper to register an RGBA texture as an ImGui texture id (for the header gradient), mirroring the existing `viewportTexture(view, sampler)` binding. (Fallback below if binding is impractical.)
- **`NodeGraphPanel` (`.cpp`):** gradient header rendering; the drag-create interaction + popup; the four right-click menus; removal of the Add toolbar palette.

## Components

### 1. Gradient header (exact UE4 look)
- Generate a small **vertical gradient texture** once at panel init — e.g. 1×64 RGBA, white at the top fading to a darker translucent bottom (the "gloss" ramp). Bind it as an ImGui texture id via a new `ImGuiLayer` helper (`void* registerTexture(rgba, w, h)` built on `ImGui_ImplVulkan_AddTexture` with an internally-created sampler/view), stored by the panel.
- In the node header draw (replacing M58's flat fill + `AddRectFilledMultiColor` sheen): draw the band with `bg->AddImageRounded(texId, a, b, uv0, uv1, tint, rounding, ImDrawFlags_RoundCornersTop)` where `tint = headerColor(category)`. The gradient texture × the category tint = the smooth UE4 header. Keep the dark canvas / raised card / light border from M58.
- **Fallback (documented, decided in the plan if binding is impractical):** keep the M58 rounded-top base fill + `AddRectFilledMultiColor` gloss — visually close, zero texture plumbing.

### 2. Drag-from-pin → type-compatible create menu
- **Headless helper** `GraphEditorModel::compatibleCreations(PortType srcType, PortDir srcDir) -> std::vector<NodeCreation>` where `NodeCreation { std::string typeName; std::string targetPort; }`. For each registered node type, find the first port that would form a valid connection with the source pin (opposite direction; `dataCompatible` for data, exec↔exec) and, if any, emit `{typeName, that port's name}`. Reuses the same compatibility predicate `connect()` uses (factor `dataCompatible` + the Out→In/exec rule into a shared `bool portsCompatible(const PortDesc& from, const PortDesc& to)` so the menu and `connect()` never diverge).
- **Panel interaction:** in the `ed::BeginCreate()` block, also call `ed::QueryNewNode(&pinId)`; when `ed::AcceptNewItem()` fires (link dropped on empty canvas), record the source `pinId` + the drop position and open a "Create Node" popup (`ed::Suspend()` → `ImGui::OpenPopup` → render → `ed::Resume()`).
- The popup lists `compatibleCreations(...)` for the dragged pin's port (resolved via the existing `resolvePin`). Selecting an entry: `model.addNode(type, dropX, dropY)` then `model.connect(...)` wiring the dragged pin to the new node's `targetPort` (respecting source direction — if the source is an Out, connect source→new.targetPort; if In, new.targetPort→source). The new node is placed at the drop position (`resetPlacement` not needed — set its editorX/Y via `addNode`).

### 3. Right-click context menus
Wrapped in `ed::Suspend()`/`ed::Resume()` (popups must be outside the canvas transform). Each frame, after `ed::End()` of node/link rendering:
- `ed::ShowBackgroundContextMenu()` → open "Add" popup: categorized submenus of all node types (`registry()->all()` grouped by `category`) + an "Add Comment" item; selection creates at the right-click canvas position.
- `ed::ShowNodeContextMenu(&nodeId)` → popup with **Delete** (and **Duplicate**: `addNode` of the same type at an offset + copy literals). If the id is in the comment namespace (M58 bit-62), offer **Delete Comment** / rename-skip.
- `ed::ShowPinContextMenu(&pinId)` → **Break links** on that pin (`disconnect`/`removeOutgoing`).
- `ed::ShowLinkContextMenu(&linkId)` → **Delete link**.

### 4. Remove the Add toolbar palette
Delete the `for (NodeTypeDesc* … ) SmallButton(...)` palette row and the "+ Comment" button from the toolbar. Keep Run / Save / Load / "Entity: …" / Load-from-entity / Assign-to-entity. Node + comment creation now happens via right-click and drag-from-pin. (The toolbar gets noticeably cleaner — the user's "Add mess" complaint.)

## Data flow
Drag a pin → `QueryNewNode` gives the source pin → on accept, popup shows `compatibleCreations` → pick → `addNode` + `connect`. Right-click empty → `ShowBackgroundContextMenu` → "Add Node" submenu → `addNode`. All mutations go through `GraphEditorModel` (so M57 undo/redo + dirty tracking cover them for free).

## Error handling
- `compatibleCreations` returns empty when nothing matches → the drag popup shows "(no compatible nodes)" and creating nothing on dismiss.
- A drag released on empty with no selection (popup dismissed) → no node created; the in-progress link is discarded by the node editor as usual.
- Connect after create is still validated by `model.connect()` (defense-in-depth: even though the menu only offers compatible types, the connect call is the authority).
- Texture binding failure (gradient) → fall back to the multicolor header (logged once).

## Testing
- **Headless `tests/test_graph_editor.cpp` (extend):**
  - `compatibleCreations(Float, Out)` includes a type with a Float/Int input (e.g. Add → "a") and excludes pure-exec-only types; the returned `targetPort` is a real input port name.
  - `compatibleCreations(Exec, Out)` includes flow types (e.g. Branch → "in") and excludes data-only types.
  - `portsCompatible` parity: for a handful of port pairs, `portsCompatible` agrees with what `connect()` accepts/rejects (guards against the menu and connect diverging).
  - Auto-wire: simulate the create path (`addNode` + `connect` to the returned `targetPort`) and assert the connection exists.
- **Visual gate:** gradient header matches the demo; drag-from-pin opens the type-filtered menu and auto-wires; the four right-click menus work; the Add toolbar palette is gone and the toolbar reads clean.

## Files
**New:** none required (helper lives in existing files).
**Modified:**
- `engine/nodes/GraphEditorModel.h` / `.cpp` — `NodeCreation` struct + `compatibleCreations(...)`; factor `portsCompatible(from, to)` from the connect/`dataCompatible` logic and use it in both.
- `engine/editor/ImGuiLayer.h` / `.cpp` — `registerTexture(rgba,w,h) -> void*` (gradient binding) [or skip if using the multicolor fallback].
- `engine/editor/NodeGraphPanel.h` / `.cpp` — gradient texture (gen + draw), drag-create popup, right-click menus, remove the Add palette.
- `tests/test_graph_editor.cpp` — `compatibleCreations` + parity + auto-wire tests.

## Notes for the plan
- Branch M59 off `main` only after M58 (PR #90) merges (M59 builds on M58's header/comment code in `NodeGraphPanel.cpp`).
- Resolve the gradient texture-binding call first (Task 1): if `ImGui_ImplVulkan_AddTexture` + an internally-created sampler/view is clean, use the texture; otherwise commit to the multicolor fallback and skip the `ImGuiLayer` change. Decide before building the header so the rest of the work isn't blocked.
- Do the headless `compatibleCreations` + `portsCompatible` refactor first (TDD), then the interactions, then the gradient, then remove the toolbar palette — so the testable core lands before the visual-gated UI.
- Keep all graph mutations routed through `GraphEditorModel` so M57 undo/redo and the M58 `"comments"` serialization keep working unchanged.
