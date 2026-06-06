# M58 — Node Editor Look v2 + Group Nodes — Design

> **Polish + small-feature milestone on the node-system track**, after M53 core → M54 panel → M55 gameplay nodes → M56 polish v1 → M57 undo/redo. Driven directly by user feedback at the M57 gate: the node editor doesn't yet read like the thedmd/imgui-node-editor UE4-blueprint example — headers are pale, text is low-res — and the user wants comment/group regions (the "Update UI" / "Update Progressbar" boxes). User chose to do all three in one combined milestone.

## Goal

Make the Node Editor look like the imgui-node-editor blueprint example: **(A)** filled category-colored header *bands* with crisp **white** titles + tuned node style; **(B)** a crisp TTF editor font replacing the blocky default; **(C)** resizable **comment/group regions** to organize a graph. No change to executable graph semantics (the headless `Graph` + `GraphEvaluator` are untouched).

## Scope (locked in brainstorming)

In: A (header bands + white titles + `ed::Style`), B (vendored Inter TTF + fallback), C (editor-only comment/group nodes with serialization + resizable rendering). Out: minimap, node search/context-menu add, reroute nodes, per-node custom colors, theme config, rich-text/markdown comments, nested groups, auto-containment (dragging a node into a group does not reparent it — groups are purely visual backdrops).

## Architecture

Three independent slices, no change to the executable graph:

- **A + C rendering** live in `engine/editor/NodeGraphPanel.cpp` (the only node-render site).
- **B** is `engine/editor/ImGuiLayer.cpp` (font load at init) + a vendored font asset.
- **C data model + serialization** lives in `engine/nodes/GraphEditorModel` (the *editor* model), **not** in `Graph` (the executable structure). Comments serialize as a sibling `"comments"` key inside `GraphEditorModel::toJson()`; the runtime loader (`iron::fromJson`, used by `LogicRuntime`/`GraphEvaluator`) only reads `"nodes"`/`"connections"` and ignores unknown keys, so Play-mode evaluation is unaffected for free.
- **Undo/redo (M57)** covers comments automatically — the graph history snapshots `graphModel.toJson()`, which now includes comments.

This milestone branches off `main` after M57 (PR #89) merges; it builds on M57's `NodeGraphPanel::focused()` and M56's pin rendering.

## Components

### A — Blueprint node look (`NodeGraphPanel.cpp`)

The node body is laid out as today (M56: title row, then two pin columns). Two changes:

1. **White title text.** Replace the current `ImGui::TextColored(headerColor(category), title)` (dark tint on dark body → pale) with `ImGui::TextColored(white, title)`. Capture the title row's screen rect: record `headerMin = ImGui::GetItemRectMin()` and, after the title (and the `Dummy` spacer), `headerMax.y`.
2. **Header band.** After `ed::EndNode()` for that node, fetch `ImDrawList* bg = ed::GetNodeBackgroundDrawList(nodeId)` and draw a filled rounded rectangle:
   - rect = `(nodePos.x, nodePos.y)` to `(nodePos.x + nodeSize.x, headerMin.y + headerHeight)` where `nodePos = ed::GetNodePosition(nodeId)`, `nodeSize = ed::GetNodeSize(nodeId)`, and `headerHeight` = the captured title-row height (title text height + the 2px dummy).
   - color = `headerColor(category)` at **full alpha** (the existing `headerColor` palette, but used as a *fill*, not text).
   - `ImDrawFlags_RoundCornersTop` with the node's rounding radius, so the band's top corners match the card and its bottom edge is square.
   - then a 1px separator line (`bg->AddLine`) at the band's bottom, slightly darker, for definition.
   - The bg draw list renders behind the node *content* but on top of the node's background fill, so the white title sits cleanly on the band. (Standard imgui-node-editor header technique — the same approach `BlueprintNodeBuilder` uses, done by hand since the builder is un-vendorable.)
3. **`ed::Style` tuning** (set once, e.g. in the panel ctor after `CreateEditor`, via `ed::GetStyle()`): `NodeRounding ≈ 6`, `NodeBorderWidth ≈ 1`, a slightly darker `StyleColor_NodeBg`, `PinRounding`, and comfortable `NodePadding`. Values chosen at the visual gate to match the example.

`headerColor` stays the M56 category palette (Event/Flow/Math/Transform/Variable/Value/Sink), now applied as the band fill. Pins, links, literals, and the position-sync loop are unchanged.

### B — Crisp font (`ImGuiLayer.cpp` + vendored asset)

- Vendor **Inter-Regular.ttf** (SIL Open Font License) under `games/11-sandbox/assets/fonts/Inter-Regular.ttf` (copied next to the exe by the sandbox's existing asset-copy step), with the OFL license text alongside it.
- In `ImGuiLayer::init`, right after `ImGui::CreateContext()` and before `ImGui_ImplVulkan_Init`, load it:
  - `ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;`
  - `ImFont* f = io.Fonts->AddFontFromFileTTF((executableDir()+"/assets/fonts/Inter-Regular.ttf").c_str(), 16.0f, &cfg);`
  - **Graceful fallback:** `if (!f) { io.Fonts->AddFontDefault(); Log::warn("ImGuiLayer: Inter font not found, using default"); }` — the editor must never fail to start because the font is missing.
- `executableDir()` is the established path helper (used for scene/asset loading). The font path is host-agnostic (relative to the exe), so it works for any editor host that copies the asset.
- ImGui 1.92 auto-manages the font texture (`ImGui_ImplVulkan_Init` 1-arg form already in use), so no manual font-atlas upload is needed.

### C — Comment / group regions

**Model (`GraphEditorModel`, editor-only):**
- Add `struct Comment { std::uint32_t id; float x, y, w, h; std::string title; };` and a `std::vector<Comment> comments_` with accessors `comments()`.
- Ops (each dirties the model, like node ops): `std::uint32_t addComment(float x, float y, float w, float h, std::string title)` (assigns an id from a **dedicated** `nextCommentId_` counter starting at 1, kept separate from the graph's node-id counter so comment and node ids are independent and serialization is self-contained), `deleteComment(id)`, `setCommentRect(id, x, y, w, h)`, `setCommentTitle(id, title)`. `nextCommentId_` is persisted in `toJson` and restored in `loadFromJson` so ids stay stable across save/load.
- Default new comment: e.g. 240×160 at the palette spawn position, title "Comment".

**Serialization (`GraphEditorModel::toJson`/`loadFromJson`):**
- `toJson()` currently `return iron::toJson(graph_);` → change to build on it: `auto j = iron::toJson(graph_); j["comments"] = <array of {id,x,y,w,h,title}>; return j;`.
- `loadFromJson(j)`: after loading the graph, read `j["comments"]` (if present) into `comments_`; tolerate absence (older files) and malformed entries (skip). Reuses the existing try/catch discipline.
- The runtime (`iron::fromJson` in `NodeGraphIO.cpp`, used by `LogicRuntime`) reads only `"nodes"`/`"connections"` — `"comments"` is silently ignored. **No runtime change.**

**Rendering (`NodeGraphPanel.cpp`):**
- For each comment, draw it as a node-editor group:
  - `ed::BeginNode(commentEditorId(c.id))` → render the title (an editable `InputText`, or a label with a rename affordance) → `ed::Group(ImVec2(c.w, c.h))` → `ed::EndNode()`.
  - Optionally a translucent fill via the comment's background draw list for the "backdrop" look (low alpha so nodes on top read clearly).
- **ID namespacing:** comment editor ids must not collide with node ids, pin ids (`node<<8|...`), or link ids (`i+1`). Use a high-bit namespace, e.g. `commentEditorId(id) = (std::uintptr_t{1} << 62) | id`. (Node ids are small uint32; pin ids are `node<<8`; link ids are small — none reach bit 62.)
- **Move/resize persistence:** after the comment loop, read `ed::GetNodePosition`/`ed::GetNodeSize` for each comment id and, on change, call `setCommentRect` (mirrors M56's node-position sync).
- **Delete:** in the existing `ed::BeginDelete` block, when a deleted `ed::NodeId` decodes to a comment namespace, call `deleteComment` instead of `model.deleteNode`.
- **Add:** a "Comment" button next to the node palette calls `addComment(...)`.
- **Z-order:** comments are drawn *before* the nodes in the draw loop so they sit behind (backdrop). imgui-node-editor groups naturally render behind regular nodes; confirm at the gate and reorder if needed.

## Testing

- **Headless (`tests/test_graph_editor.cpp`, extend):**
  - `addComment` → `comments()` has it with the right rect/title; `setCommentRect`/`setCommentTitle` mutate it; `deleteComment` removes it; each dirties the model.
  - `toJson` → `loadFromJson` round-trip preserves comments (count + a sampled rect + title).
  - **Runtime tolerance:** a graph JSON that contains a `"comments"` array still loads via `iron::fromJson` (or evaluates via the existing evaluator test path) — asserts comments don't break executable loading. (Add to `test_graph_editor` or `test_logic_runtime`, whichever already exercises `fromJson`.)
- **Visual gate:** A (filled colored header bands + crisp white titles + rounded cards), B (sharp text across the whole editor), C (add a comment, type a title, resize/move it, nodes sit on top, it persists through Save/Load and undo/redo).

## Risks / notes

- **Font sourcing + licensing:** use Inter (OFL) and commit the license text; the missing-file fallback keeps the editor running if the asset isn't copied.
- **Header band z-order / geometry:** the band height depends on the captured title-row rect; verify the rect is captured in screen space consistent with `GetNodePosition` (both screen-space within the canvas). Tune at the gate.
- **Comment id namespacing:** the bit-62 namespace must stay clear of node/pin/link encodings (it does for realistic graph sizes); document the invariant in code.
- **logicGraph round-trip:** M55 stores `graphModel.toJson().dump()` on the entity and the runtime parses it — the parser-tolerance test guards that comments don't regress Play.
- **Undo coverage:** comments ride in `toJson`, so M57 undo/redo covers add/delete/move/resize/retitle with no extra wiring — but confirm the graph coalescing (`graphModel.dirty()`) fires on comment ops (the ops must set the dirty flag).

## Files

**New:**
- `games/11-sandbox/assets/fonts/Inter-Regular.ttf` (+ `OFL.txt`).

**Modified:**
- `engine/nodes/GraphEditorModel.h` / `.cpp` — `Comment` struct, `comments_`, add/delete/setRect/setTitle, `toJson`/`loadFromJson` comment support (+ dirty on comment ops).
- `engine/editor/NodeGraphPanel.cpp` — white titles + header band via `GetNodeBackgroundDrawList`, `ed::Style` tuning, comment rendering/add/delete/persist, comment-id namespacing.
- `engine/editor/ImGuiLayer.cpp` — load Inter TTF with fallback.
- `games/11-sandbox/CMakeLists.txt` — ensure the fonts asset dir is copied next to the exe (if not already covered by a glob).
- `tests/test_graph_editor.cpp` (and possibly `tests/test_logic_runtime.cpp`) — comment unit tests + runtime-tolerance assertion.

## Notes for the plan

- Branch M58 off `main` only after M57 (PR #89) has merged, so `NodeGraphPanel::focused()` and M57's panel edits are present (avoids a merge in `NodeGraphPanel.{h,cpp}`).
- Do A and B first (pure presentation, fast visual confirmation), then C (model → serialization → rendering), so the big visual win is verifiable early even within the one milestone.
- Keep `headerColor` as the single source of category color; A reuses it as a fill, so no palette duplication.
