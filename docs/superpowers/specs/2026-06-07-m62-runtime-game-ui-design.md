# M62 — Runtime Game UI (design)

**Date:** 2026-06-07
**Status:** Approved (brainstorm), pending implementation plan
**Track:** "Ship a simple game" pillar #1 (UI). Foundation for node-track #6 (UI nodes).

## Goal

Give the engine a real **runtime, in-game UI system** — a retained, data-driven
widget tree with layout, interactivity, and crisp text — so a game can present a
**main menu**, an **in-game HUD**, and a **pause overlay**. Today the only UI is
`engine/ui/Hud` (a manual, pixel-positioned, monospace-bitmap screen-space
batcher with zero layout or interactivity), used directly by six games.

This is the weakest pillar and is consumed by every later milestone (gameplay
needs menus, Steam needs lobby/friends UI, shipping needs a title screen).

## Scope decisions (locked at brainstorm)

- **Runtime-first.** Build the widget system + a C++ builder API + JSON
  serialization + the three demo screens. **No** editor drag-drop UI builder in
  M62 — it is a separate milestone that sits on top of this data model.
- **Crisp TTF text.** Bake Roboto (already vendored for the editor) into a
  proportional glyph atlas via `stb_truetype` (already on the include path via
  vcpkg). Menus/HUD render sharp, not blocky.
- **Demo screens:** Main Menu + In-Game HUD + Pause Overlay. (Settings, with its
  slider/checkbox widgets, is deferred.)
- **Demo host:** a small **new** game `games/12-ui-arena` (single `main.cpp`),
  not the editor sandbox (which has no menu/play flow) and not net-shooter
  (avoids entangling networking).

## Architecture

Mirror the node-editor split that worked well: a **headless, retained-mode model**
that is pure data and unit-testable, rendered through the **existing
`HudBatch` → `VkHud`/`GLHud` path** (no new renderer). The widget tree is the
single source of truth, exactly like `GraphEditorModel` is for node graphs.

### Modules (all new under `engine/ui/`, alongside the untouched `Hud`)

| File | Responsibility | Depends on |
|---|---|---|
| `UiElement.h` | The widget node struct + enums (data only) | `math/Vec`, `render/Handles` |
| `UiTree.h/.cpp` | Owns the element tree; add/remove/find; root rect | `UiElement` |
| `UiLayout.h/.cpp` | Layout pass: anchor + box model + Stack flow → screen rects | `UiTree` |
| `UiInput.h/.cpp` | Input pass: hit-test, hover/press/click, focus nav, modal capture | `UiTree`, `UiLayout` output |
| `UiRender.h/.cpp` | Render pass: laid-out tree → `HudBatch` quads + text | `UiTree`, `HudBatch`, `FontAtlas` |
| `UiStack.h/.cpp` | Screen stack (push/pop, modal flag, per-screen input gating) | `UiTree` |
| `UiSerialize.h/.cpp` | JSON load/save of a `UiTree` | `UiTree`, nlohmann/json |
| `FontAtlas.h/.cpp` | Bake a TTF into a proportional glyph atlas via `stb_truetype` | `render` (texture upload), `stb_truetype` |

`Hud`, `BitmapFont`, `BuiltinFont` stay exactly as they are. The six games using
`Hud` are **not** migrated in M62 (YAGNI; keep the milestone focused).

## Data model

One element type, a tree of them (children owned by parent, stable ids):

```
struct UiElement {
    UiId        id;                 // 1-based; 0 invalid
    UiKind      kind;               // Panel | Label | Image | Button | Bar
    Anchor      anchor;             // TopLeft..BottomRight (9) | Stretch
    Vec2        offset;             // px from the anchor
    Vec2        size;               // px (ignored when anchor == Stretch)
    Vec4        color;              // panel bg / label text / image tint / bar fill
    bool        visible = true;

    // Stack layout (Panel only): if dir != None, children flow along it.
    StackDir    stack = StackDir::None;   // None | Vertical | Horizontal
    float       spacing = 0.0f;

    // kind-specific:
    std::string text;               // Label, Button caption
    float       fontPx = 18.0f;     // Label/Button text size
    TextureHandle texture;          // Image
    float       value = 0.0f;       // Bar fill fraction 0..1
    Vec4        trackColor;         // Bar background
    std::uint32_t actionId = 0;     // Button → game-defined action code

    std::vector<UiElement> children;
};
```

A C++ builder API constructs trees readably (e.g. `ui::panel(...).stackV(8).add(button("Play", ACT_PLAY))...`). Exact builder ergonomics are an implementation detail; the struct above is the contract.

### Anchors & layout

- **Anchor** resolves an element's rect against its **parent's** resolved rect:
  the 9 presets place the element's corresponding point; `Stretch` fills the
  parent (minus offset as inset). This is the UMG/uGUI mental model, trimmed to
  what menus/HUDs need — no full flexbox, no percentage units beyond Stretch.
- **Stack** (Panel only): children are placed sequentially along `stack` with
  `spacing`, each still honoring its cross-axis anchor. This is what a vertical
  button list (Main Menu, Pause) uses.
- Layout is a pure function `layout(tree, screenSize) -> map<UiId, Rect>`; no
  side effects, fully unit-testable.

## The three passes (per frame)

1. **Layout** — produce screen rects (above).
2. **Input** — given the rects + mouse pos/buttons + nav keys:
   - hover = topmost element whose rect contains the cursor;
   - press/click on Buttons → collect fired `actionId`s for the game to handle;
   - **focus navigation:** Up/Down (or gamepad d-pad, same axis) moves focus
     among focusable elements in the active screen; Enter/A activates focused;
   - **modal capture:** `UiStack` marks a screen modal; input does not reach
     screens beneath it (so the game world ignores clicks behind a pause menu).
   - Output: list of fired actions + current hover/focus (for render highlight).
3. **Render** — walk visible, laid-out elements and emit into a `HudBatch`:
   Panels/Bars → colored quads (`whiteTexture`), Images → textured quads,
   Labels/Buttons → text quads via `FontAtlas`. Hover/focus is a tint/overlay
   on Buttons. The `HudBatch` is handed to the existing renderer HUD path.

`UiStack` owns N screens; each frame it lays out + renders bottom-to-top and
routes input top-to-modal.

## Text (FontAtlas)

- At load, `FontAtlas::bake(ttfBytes, pixelHeight)` uses `stb_truetype` to
  rasterize the printable ASCII range into a single 8-bit coverage atlas,
  recording per-glyph `{uvMin, uvMax, sizePx, bearingPx, advancePx}`. The atlas
  is uploaded as a texture via the renderer (same mechanism as other UI
  textures).
- Text layout advances by `advancePx` (proportional), with the baseline/bearing
  applied per glyph — replacing the monospace-grid assumption in the current
  `Hud::build` text path. The new `UiRender` text routine implements this; the
  legacy `Hud` keeps its monospace routine.
- One baked size is sufficient for M62 (HUD + menu sizes); rendering at other
  sizes scales the quads (mild blur accepted, as with bitmap text today). SDF /
  multi-size atlases are out of scope.

## Demo host — `games/12-ui-arena`

A single `main.cpp` following the existing game template (`Application` +
`createRenderer` + camera):

- Boots into the **Main Menu** screen (title + Play / Quit). Play transitions to
  the game screen; Quit exits.
- **Game screen:** a trivial scene (a spinning prop + skybox) with the **HUD**
  screen on top — health bar, ammo/score text, timer — bound to fake values that
  tick over time (and a key to "take damage") so the bars visibly move. No real
  gameplay, no networking.
- **Esc** pushes the **Pause** overlay (modal, dims the scene) with Resume /
  Quit-to-Menu. Resume pops it; the world is frozen while paused (input captured).

This exercises the entire system end-to-end for the visual gate: buttons +
hover/focus, anchored live-bound bars, modal stacking, screen transitions, crisp
text — all driven from the C++ builder API and round-trippable through
`UiSerialize`.

## Testing

New `tests/test_ui` (headless, no window), covering:

- **Layout:** each anchor preset resolves to the expected rect against a known
  parent; Stretch insets by offset; nested anchors compose; Stack places
  children with spacing along the axis.
- **Input:** hit-testing returns the topmost element; a click inside a Button's
  rect fires its `actionId`, outside does not; focus nav cycles the focusable
  set and wraps; Enter activates the focused button.
- **Modal capture:** with a modal screen on top, input does not reach a button on
  the screen beneath.
- **Serialization:** a built tree round-trips through `UiSerialize`
  (`load(save(tree)) == tree`), including children, anchors, and kind-specific
  fields.

TTF rasterization correctness and the three screens are validated at the user
visual gate (run `games/12-ui-arena`).

## Out of scope (future milestones)

- Editor drag-drop UI builder (its own milestone; edits this same data model).
- UI nodes (node-track #6) — also targets this data model.
- Slider / Checkbox / Settings screen.
- Text wrapping, rich text, multiple font sizes/atlases, SDF text.
- Migrating the six existing `Hud`-based games to the new system.
- Localization, DPI scaling, animation/tweening.

## Risks / notes

- **HudBatch text path change.** Moving from monospace-grid to proportional
  glyph metrics touches text-quad generation. Mitigated by adding a *new*
  `UiRender` text routine rather than altering `Hud::build` — the legacy path is
  untouched, so the six existing games keep working unchanged.
- **stb_truetype source.** Available via vcpkg include path
  (`vcpkg_installed/.../stb_truetype.h`). If CMake include resolution is awkward,
  vendor the single header into `third_party/stb` alongside `stb_image`.
- **GL backend.** `GLHud` exists; M62 targets Vulkan (canonical `build-vk`) per
  the Vulkan-only direction. The new `UiRender` produces a backend-agnostic
  `HudBatch`, so GL is unaffected and not a deliverable.
