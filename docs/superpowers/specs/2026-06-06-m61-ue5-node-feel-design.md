# M61 — UE5 Node Editor Feel — Design

> **Node-system polish milestone**, after M53 core → M54 panel → M55 gameplay nodes → M56 polish v1 → M57 undo → M58 look v2 → M59 UX v2 (gradient header + context menus) → M60 dark theme. Driven by user feedback: the node editor should feel like UE5 Blueprints — searchable/grouped create menu, per-type header icons, node subtitles, a glassy translucent header gradient, and dev-only tags. Designed comprehensively so the whole reference set lands in one plan.

## Goal

Make the node editor read like UE5 Blueprints: **(1)** a searchable, collapsible-category-grouped create menu; **(2)** per-type header **icons** (⚡ events, *f* functions, …) via a vendored icon font; **(3)** node **subtitles** ("Target is …"); **(4)** a fancier, **more-transparent header gradient** + a darker text-contrast region; **(5)** a subtle **glass** sheen on the card; **(6)** **dev-only** striped tags. One milestone, phased tasks, one PR.

## Scope (locked in brainstorming)

In: all six above. Icon source = a **vendored icon font** (Fork Awesome). Delivery = **one milestone, phased tasks**. Out (YAGNI): node tooltips/help text, per-instance node colors, fuzzy-ranked search (plain case-insensitive substring), search-result thumbnails beyond the category glyph, multiple icon sets. No change to executable graph semantics or per-graph JSON; all mutations stay routed through `GraphEditorModel` (M57 undo + M58 comments intact).

## Architecture

Three layers, each with a clear boundary:

- **Model** (`engine/nodes`): `NodeTypeDesc` gains optional trailing `std::string subtitle{}` + `bool devOnly=false` (defaulted, so existing `registerType({...})` aggregate-inits keep compiling). Built-in + gameplay nodes author subtitles (and one representative `devOnly`). `catalogToJson` emits the new fields (AI-contract completeness). A pure helper `buildCreateList(registry, query, compatFilter)` produces the grouped/filtered create list — **headless, unit-tested**.
- **Icon font** (`engine/editor/ImGuiLayer`): merge Fork Awesome into the editor atlas; a `nodeCategoryIcon(category)` map → glyph string. Category-driven (no per-type icon field).
- **Rendering** (`engine/editor/NodeGraphPanel`): the create-menu popup driver (search box + collapsible groups), and the header/card draw (icon, subtitle, contrast region, translucent gradient, glass sheen, dev tag). **Visual-gated.**

## Components

### 1. Icon font (Fork Awesome)
- Vendor `forkawesome-webfont.ttf` (SIL OFL) into `games/11-sandbox/assets/fonts/`, and the codepoint header `IconsForkAwesome.h` (from the standard `juliettef/IconFontCppHeaders`) into `third_party/`.
- In `ImGuiLayer::init`, after loading Roboto-Medium, **merge** the icon font into the same atlas:
  ```cpp
  static const ImWchar kFaRange[] = { ICON_MIN_FK, ICON_MAX_16_FK, 0 };
  ImFontConfig icfg; icfg.MergeMode = true; icfg.PixelSnapH = true;
  icfg.GlyphMinAdvanceX = 16.0f;  // monospace-ish icons
  io.Fonts->AddFontFromFileTTF(faPath.c_str(), 15.0f, &icfg, kFaRange);
  ```
  Graceful fallback: if the FA file is missing, skip the merge (icons just don't render; text/`#?` placeholder is acceptable) — log a warning, never fail init.
- `nodeCategoryIcon(const std::string& category) -> const char*` (in `ImGuiLayer` or a small editor header): Event→`ICON_FK_BOLT`, Math→`ICON_FK_CALCULATOR`, Flow→`ICON_FK_SITEMAP`, Transform→`ICON_FK_ARROWS`, Variable→`ICON_FK_CUBE`, Value→`ICON_FK_HASHTAG`, Sink→`ICON_FK_SIGN_OUT`, default→`ICON_FK_SQUARE_O`. (Map to the actual macro names present in the vendored header; verify at implementation.)

### 2. Node metadata (`NodeTypeDesc`)
- Add trailing `std::string subtitle{};` and `bool devOnly = false;` after `isEntry`. Existing positional `registerType({name, cat, ports, fn, isEntry})` calls remain valid (new fields default).
- Author subtitles for built-in + gameplay nodes — short UE5-style descriptors, e.g. Entry→"Graph entry point", Branch→"Flow control", Sequence→"Run outputs in order", Compare→"Compare two values", Add→"a + b", Const→"Literal value", SetOutput→"Write a graph output", OnTick→"Every frame", and the gameplay Transform/Variable nodes similarly. (Author concise, accurate lines; empty for any node where a subtitle adds nothing.)
- Tag one representative node `devOnly = true` (e.g. a logging/debug-ish node if one exists, else `SetOutput` as an illustrative stand-in) so the dev-tag renders at the gate; the flag is then infrastructure for future debug nodes.
- `catalogToJson` (NodeRegistry.cpp): add `"subtitle"` and `"devOnly"` to each entry (additive — AI consumers ignore unknown/extra fields safely).

### 3. Searchable, grouped create menu
- Pure core: `std::vector<NodeCreateGroup> buildCreateList(const NodeRegistry&, std::string_view query, const std::vector<std::string>* compatTypeFilter)` where a group is `{ std::string category; std::vector<const NodeTypeDesc*> types; }`. Empty query → all types grouped by category (sorted). Non-empty query → a single "Results" group of types whose `typeName` or `subtitle` contains the query (case-insensitive). `compatTypeFilter` (non-null for drag-from-pin) restricts to those type names. **Unit-tested.**
- Driver `drawCreateMenu(model, query buffer, optional source pin + targetPort map, dropPos)` in `NodeGraphPanel`: a `InputText` search box (auto-focused on open) at the top; below it, for each group, a `CollapsingHeader(category)` (open by default when searching) listing `MenuItem(icon + typeName)` with the subtitle as dim text. Selecting a type creates it at `dropPos` and (drag-from-pin) auto-connects via the stored `targetPort` (reuse M59 `compatibleCreations` for the compat set + ports). 
- Reused by BOTH `##bg_ctx` "Add Node" (no compat filter) and the drag-from-pin `##create_node_popup` (compat-filtered). Replaces M59's flat `registry()->all()` submenu and flat `compatibleCreations` list.

### 4. Header polish (icon + subtitle + contrast + transparent gradient)
In the `NodeGraphPanel` node draw (building on M58/M59's header band):
- **Icon + title:** render `nodeCategoryIcon(category)` glyph then the title (white) on the same row; capture the row rect.
- **Subtitle:** if `t->subtitle` non-empty, render it below the title in smaller, dim text (`kTextDim`-ish). It becomes part of the header region; the band auto-sizes to cover icon+title+subtitle (extend the captured header-bottom to include the subtitle row).
- **Text-contrast region:** draw a subtle dark translucent rounded rect behind the icon+title+subtitle (within the header band) so they pop on the colored gradient.
- **More-transparent gradient:** lower the gloss-texture tint alpha (M59 used `255`); make the band a translucent colored sheen that fades toward the body (e.g. tint alpha ~150 at top → blends into the near-black body), so the header reads as glass-colored, not a solid bar.

### 5. Glass card sheen
Over the near-black, ~91%-opaque body (from M60): via the node background draw list, add a faint top-down white sheen (alpha ~10 at the very top → 0 a third of the way down) and a 1px brighter inner top-edge highlight — the subtle glossy "glass" look. Keep it subtle so text stays readable.

### 6. Dev-only striped tag
For types with `devOnly==true`, draw a short yellow/black diagonal-striped strip along the node's bottom edge (bg draw list: a few alternating-color quads clipped to the node width) with a small "DEV ONLY" label. Minimal; one representative node tagged so it's visible.

## Data flow
Create: search/group via `buildCreateList` → `MenuItem` select → `model.addNode(type, dropPos)` (+ `model.connect` for drag-from-pin). Render: per node, look up `NodeTypeDesc` (subtitle/devOnly/category) via the registry → draw icon/subtitle/band/glass/tag. All graph edits through `GraphEditorModel`.

## Error handling
- Missing FA font → skip the merge, log a warning, icons absent (editor still works). 
- Empty subtitle → no subtitle row (header sizes to title only).
- `buildCreateList` with no matches → an empty list (menu shows "(no matches)").
- Icon macro names that differ in the vendored header → mapped at implementation against the actual `IconsForkAwesome.h`.

## Testing
- **Headless (`tests/`):**
  - `buildCreateList`: empty query groups by category (every registered type present, grouped); a query filters case-insensitively on typeName + subtitle; a `compatTypeFilter` restricts results; no-match → empty.
  - `catalogToJson`: emitted entries include `"subtitle"` and `"devOnly"`; a node with an authored subtitle round-trips its value; `devOnly` true for the tagged node.
- **Visual-gated:** icons in headers, subtitles, translucent gradient + contrast region, glass sheen, dev-only strip, and the search/grouped create menu (type to filter; expand categories; drag-from-pin shows only compatible types grouped).

## Files
**New:**
- `games/11-sandbox/assets/fonts/forkawesome-webfont.ttf` (+ OFL license text).
- `third_party/IconsForkAwesome.h` (codepoint macros).

**Modified:**
- `engine/nodes/NodeRegistry.h/.cpp` — `subtitle`/`devOnly` fields; `catalogToJson` emits them.
- `engine/nodes/BuiltinNodes.cpp`, `engine/gameplay/GameplayNodes.cpp` — author subtitles + one `devOnly`.
- `engine/nodes/GraphEditorModel.h/.cpp` (or a small `NodeCreateList.{h,cpp}`) — `buildCreateList` pure helper + `NodeCreateGroup`.
- `engine/editor/ImGuiLayer.cpp` — merge the icon font; `nodeCategoryIcon`.
- `engine/editor/NodeGraphPanel.h/.cpp` — search/grouped create menu (replaces M59 flat lists); header icon/subtitle/contrast/gradient; glass sheen; dev tag.
- `tests/test_graph_editor.cpp` (and/or `tests/test_node_graph.cpp`) — `buildCreateList` + `catalogToJson` tests.

## Notes for the plan (phasing)
Order so the testable core lands first and the editor stays buildable each task:
1. **Model + catalog** (`subtitle`/`devOnly` + `catalogToJson` + author subtitles) — TDD.
2. **`buildCreateList`** pure helper — TDD.
3. **Icon font merge** + `nodeCategoryIcon` (verify FA macro names against the vendored header; graceful fallback).
4. **Search/grouped create menu** wired into `##bg_ctx` + drag-from-pin (replacing M59 flat lists).
5. **Header icon + subtitle + contrast + transparent gradient.**
6. **Glass sheen + dev-only tag.**
7. **Build + tests + visual gate + PR.**
- Branch M61 off `main` after M60 (PR #92) merges (it builds on M59/M60). M60 is merged.
- Fork Awesome: vendor the TTF + `IconsForkAwesome.h` from `juliettef/IconFontCppHeaders` (matching FA version). Confirm the exact `ICON_MIN_FK`/`ICON_MAX_16_FK` range macros + glyph names the header defines; map categories to glyphs that actually exist.
- Keep all graph mutations through `GraphEditorModel`; the create menu only calls `addNode`/`connect`.
