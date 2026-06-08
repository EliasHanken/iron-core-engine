# M63 — Rich Inventory UI (Design)

**Date:** 2026-06-08
**Branch:** `m63-rich-inventory-ui`
**Depends on:** M62 (runtime widget tree, layout/input/render, FontAtlas, UiStack) — merged to `main` as `4b2a770`.
**Leads to:** M64 (networked shared chest — reuses this milestone's `Inventory` model server-side and its looting UI client-side).

## Goal

A single-player chest-looting screen that proves the engine can build "complex" game UI, not just text HUDs. Concretely: two side-by-side item grids (a chest and the player's backpack), drag-and-drop **and** double-click to transfer item stacks between them, the chest grid scrolls when it overflows, and everything is dressed in 9-slice chrome (rounded, bordered panels/slots/buttons) instead of flat rectangles. This directly answers the "boring, only text" gap while building the exact substrate M64 needs.

This builds entirely on M62's headless widget tree and the existing `HudBatch` → `drawHud` render path. No new renderer. Legacy `Hud`/`BitmapFont` and the M62 widget set stay untouched and working.

## Non-goals (YAGNI)

- No data-binding framework — the demo rebuilds the widget tree from the model each frame (M62's proven pattern; ids are deterministic).
- No UI animations/transitions.
- No real player locomotion — press **E** to open/close the looting screen.
- No mouse-picking of the chest mesh (clicking the 3D chest to open is a stretch goal, not required).
- No item rotation / weight / equipment slots / tooltips. Just slots, stacks, and transfer.
- No scissor-based GPU clipping (CPU clip keeps it headless/testable — see §3.2).

## Architecture overview

Three layers, mirroring M62's separation of pure-logic units:

1. **`engine/ui/`** — extend the existing widget system with the containers, styling, and interactions needed for inventories. All headless and unit-testable.
2. **`engine/game/Inventory.{h,cpp}`** — a headless item/stack/inventory model with transfer logic. The UI binds to it; M64 reuses it server-side.
3. **`games/13-loot/`** — the demo that wires a 3D scene + two `Inventory` instances to the looting UI.

Per-frame data flow (unchanged shape from M62):

```
carry { scrollOffsets, focus, dragState }   (persisted across the per-frame rebuild)
  → rebuild widget tree from the two Inventory models   (deterministic ids)
  → layoutUi(root, screen, scrollOffsets)                (grid wrap; apply scroll offset)
  → updateUi(root, rects, input, carried state)          (hit-test, drag, wheel)
        emits: UiDropEvent{source,target}, UiQuickTransfer{source}, scroll deltas
  → game applies events to the Inventory models          (move / merge / quick-transfer)
  → renderUi(...)  → HudBatch  → renderer.drawHud(...)    (9-slice quads, slot icons, drag ghost on top)
```

## 1. Inventory model — `engine/game/Inventory.{h,cpp}`

Pure C++, no rendering or input dependency. Unit-tested in `tests/test_inventory.cpp`.

```cpp
namespace iron {

using ItemId = std::uint32_t;          // 0 = "none"/empty
constexpr ItemId kNoItem = 0;

struct ItemDef {                        // static definition (game-owned table)
    ItemId id = kNoItem;
    std::string name;
    int maxStack = 1;                   // 1 = non-stackable
    TextureHandle icon = kInvalidHandle;// game uploads; engine just carries it
};

struct ItemStack {                      // occupant of one slot
    ItemId item = kNoItem;
    int count = 0;                      // 0 ⇔ empty slot
    bool empty() const { return item == kNoItem || count == 0; }
};

class Inventory {
public:
    explicit Inventory(int slotCount);
    int size() const;
    const ItemStack& at(int slot) const;

    // Add into the first mergeable/free slot(s), respecting maxStack of `def`.
    // Returns leftover count that didn't fit (0 = all placed).
    int addItem(const ItemDef& def, int count);

    // Remove (up to) `count` from a slot; returns how many were removed.
    int removeAt(int slot, int count);

    // Move a whole/partial stack from (src,srcSlot) to (this,dstSlot):
    //  - empty dst → place
    //  - same-item dst → merge up to maxStack, leftover stays at src
    //  - different-item dst → swap
    // Returns true if anything changed.
    static bool moveTo(Inventory& src, int srcSlot,
                       Inventory& dst, int dstSlot, const ItemDefTable& defs);

    // Auto-transfer the stack at (src,srcSlot) into dst's first mergeable/free
    // slot(s). Returns true if anything moved.
    static bool quickTransfer(Inventory& src, int srcSlot,
                              Inventory& dst, const ItemDefTable& defs);
private:
    std::vector<ItemStack> slots_;
};

} // namespace iron
```

`ItemDefTable` is a small lookup (`std::unordered_map<ItemId, ItemDef>` wrapper, or a `std::function`/`std::span` accessor) so the model can read `maxStack` during merges without owning the defs. The game owns the table.

Test surface: place into empty/partial slots with stacking; overflow returns leftover; `moveTo` for empty/merge/swap cases; `quickTransfer` to free vs mergeable vs full destination; remove partial/whole.

## 2. UI widget extensions — `engine/ui/`

### 2.1 `UiElement` (extend, `UiElement.h`)
- New kinds: `UiKind::Grid`, `UiKind::ScrollBox`. (Slots are composed from existing kinds + flags — see below — so no `Slot` kind is needed.)
- New fields:
  - `int gridCols = 0;` — columns for `Grid`.
  - `float scrollOffset = 0;` — current vertical scroll (px) for `ScrollBox`; supplied per-frame from carried state.
  - `Vec4 nineSliceMargin{0,0,0,0};` — L/T/R/B border insets (px) for 9-slice Panel/Image; all-zero ⇒ plain quad (back-compat).
  - `bool draggable = false;` — element initiates a drag when pressed + moved.
  - `bool dropTarget = false;` — element can receive a drop.
  - `std::uint32_t userData = 0;` — payload (the game encodes `(containerId<<16)|slotIndex`).
- New builders: `uiGrid(anchor, offset, size, cols, spacing)`, `uiScrollBox(anchor, offset, size, spacing)`, `uiImage9(anchor, offset, size, texture, margin, color)`. A slot is built by the game as a `uiImage9(...)` (9-slice tile) with `draggable`/`dropTarget`/`userData` set and an icon `uiImage` + count `uiLabel` as children — but a convenience `uiSlot(...)` builder is provided to assemble that.
- `uiEqual` / `uiAssignIds` / serialization extended to cover the new fields (serialization round-trips them; ids/textures still not persisted, per M62).

### 2.2 `UiLayout` (`UiLayout.cpp`)
- **Grid:** children are laid out left-to-right, wrapping every `gridCols` into a new row, using each child's `size` plus the grid's `spacing` for both axes. The grid's own content height grows with row count (used by ScrollBox to know overflow).
- **ScrollBox:** lays out its child content normally, then translates all descendants up by `scrollOffset` and records a **clip rect** = the ScrollBox's own screen rect. The clip rect for each laid-out element is stored alongside its `Rect` (e.g. `UiLayoutMap` value becomes `{Rect rect; Rect clip;}`, where `clip` defaults to "no clip"/full-screen for non-scrolled elements). `scrollOffset` is clamped to `[0, contentHeight - viewportHeight]` during layout so out-of-range carried values self-correct.

### 2.3 `UiInput` (`UiInput.{h,cpp}`)
Extend `UiInputState` with `mouseDown` (held, distinct from the existing one-shot `mousePressed`), `mouseReleased`, `doubleClick`, and `wheel` (float, +up).

Add carried `UiDragState { bool active; UiId sourceId; std::uint32_t sourceUserData; Vec2 grabOffset; }`, threaded in/out like focus.

`UiInputResult` gains:
- `std::optional<UiDropEvent> drop;` where `UiDropEvent { std::uint32_t source; std::uint32_t target; }` (userData of source slot and the drop-target slot under the cursor at release).
- `std::optional<std::uint32_t> quickTransfer;` (source userData) on double-click of a draggable element.
- `std::vector<std::pair<UiId,float>> scrollDeltas;` — wheel applied to the hovered `ScrollBox` (the game/UiStack accumulates these into the carried `scrollOffsets`).
- `UiDragState drag;` — updated state to carry to next frame.

Behavior:
- **Drag start:** `mouseDown` over a `draggable` element + cursor moved past a small threshold (e.g. 4px) ⇒ `drag.active = true`, capture `sourceId`/`sourceUserData`/`grabOffset`.
- **Drag end:** on `mouseReleased` while dragging ⇒ if the topmost element under the cursor is a `dropTarget`, emit `UiDropEvent{sourceUserData, targetUserData}`; clear drag.
- **Double-click** on a `draggable` element (not dragging) ⇒ emit `quickTransfer = sourceUserData`.
- **Wheel** while hovering a `ScrollBox` (or a descendant) ⇒ push `{scrollBoxId, -wheel*step}` into `scrollDeltas`.
- Existing button click/hover/focus behavior is preserved.

### 2.4 `UiRender` (`UiRender.cpp`)
- **9-slice:** when `nineSliceMargin` is non-zero, emit **9 quads** from the element's texture: 4 fixed-size corners, 2 stretched horizontal edges, 2 stretched vertical edges, 1 stretched center, with UVs split by the margins (assumes the source texture's border matches the margins in texel space; for simplicity the source border fraction is taken equal to the margin px against a known atlas tile — documented as "border texels = margin px"). Plain `nineSliceMargin == 0` path is the existing single quad.
- **CPU clipping:** a helper `clipQuad(rectMin,rectMax, uvMin,uvMax, clip) -> optional<quad>` intersects a quad with a clip rect and proportionally remaps UVs; fully-outside ⇒ dropped. Every quad emitted for an element whose layout entry carries a non-full `clip` rect is passed through `clipQuad` (covers panels, icons, **and glyphs**, so partially-scrolled rows clip cleanly mid-tile/mid-glyph).
- **Drag ghost:** after all normal elements, if `drag.active`, the dragged slot's icon is drawn at the cursor (offset by `grabOffset`), semi-transparent, on top of everything (appended last so it sorts above).
- Existing panel/label/image/bar/button rendering unchanged.

### 2.5 `UiStack` (`UiStack.h/.cpp`)
- Carry a `std::unordered_map<UiId,float> scrollOffsets_` and a `UiDragState drag_` across the per-frame rebuild, applied to layout/input and updated from `UiInputResult` — analogous to the existing `topFocus()/setTopFocus()` focus carrying. `update()` consumes `scrollDeltas` into `scrollOffsets_`. Accessors mirror the focus ones so the demo (and M64) don't poke internals.

## 3. Render path notes

### 3.1 Reuse, don't extend the renderer
All new visuals (9-slice, clipping, drag ghost) are produced as ordinary `HudVertex` quads in `UiRender`. `VkHud`/`drawHud` are untouched. `drawHud` is still called **before** `endFrame` (M62 constraint).

### 3.2 Why CPU clipping instead of GPU scissor
A scissor rect per `HudDrawGroup` would be the "engine" way, but it forces a renderer change (`VkHud` + the batch struct) and is **not unit-testable headlessly**. CPU quad-clipping with UV remap is exact for axis-aligned quads (all our UI is axis-aligned), keeps `UiRender` pure, and is covered by `test_ui`. Accepted trade-off; revisit only if a future widget needs rotated/over-draw clipping.

## 4. Demo — `games/13-loot/`

- Scene: a chest mesh (reuse a cube/crate) on a ground plane, orbit camera (like `12-ui-arena`).
- Item table: ~6 `ItemDef`s (sword, potion, coin, scroll, gem, key) with small icon PNGs in `games/13-loot/assets/items/`. Icons uploaded via `renderer.createTexture(...)`. (If sourcing icons is slow, fall back to solid-color 9-slice tiles + the item's first letter — the engine is icon-agnostic.)
- A `chest` Inventory (seeded with items, more than fit ⇒ scrolls) and a `backpack` Inventory (mostly empty).
- Press **E** near/at the chest to toggle the looting screen (a modal `UiStack` screen over a thin HUD). While open, the world is frozen (`topIsModal()`), mouse drives drag/drop, double-click quick-transfers, wheel scrolls the chest grid. Press **E** or **Esc** to close.
- 9-slice chrome: a panel border PNG and a slot-tile PNG in `games/13-loot/assets/ui/` give the rounded/bordered look; slot hover brightens (reuse M62 hover).

## 5. Testing

- **`tests/test_inventory.cpp`** (new): `addItem` stack/merge/overflow; `removeAt` partial/whole; `moveTo` place/merge/swap; `quickTransfer` to free/mergeable/full.
- **`tests/test_ui.cpp`** (extend): grid wrap positions for N items / `gridCols`; ScrollBox clamps offset + records clip rect; `clipQuad` UV-remap math (fully in / fully out / partial); 9-slice expands to 9 quads with correct corner sizes; drag start threshold + `mouseReleased`-over-dropTarget emits `UiDropEvent` with right source/target; double-click emits `quickTransfer`; wheel over ScrollBox produces a scroll delta.
- Build both `test_inventory` and `test_ui`; full suite green before PR (M62 left the suite green; this adds `test_inventory` plus new `test_ui` cases).

## 6. Task breakdown (for the implementation plan)

Rough TDD task slicing (final shape decided in writing-plans):
1. `Inventory` model + `test_inventory`.
2. `UiElement` extensions (new kinds/fields/builders/`uiEqual`/ids/serialize) + tests.
3. `UiLayout` Grid + ScrollBox (clip rect, offset clamp) + tests.
4. `UiRender` 9-slice expansion + `clipQuad` + clipping wiring + tests.
5. `UiInput` drag/drop + double-click + wheel + carried `UiDragState` + tests.
6. `UiStack` scroll-offset + drag-state carrying + accessors + tests.
7. `UiRender` drag-ghost overlay + tests.
8. `games/13-loot` demo (scene, item table, two inventories, looting screen, E-to-open) + CMake registration + asset copy.

## 7. Risks / open questions

- **9-slice source convention** ("border texels = margin px") is a simplification; if it looks wrong at the visual gate we tune the UV split. Low risk.
- **Slot identity encoding** in `userData` (`container<<16 | index`) caps at 65k slots/container — fine forever for this use.
- **Double-click detection** needs a frame-time/click-timer; `UiInputState.doubleClick` is computed by the host (the demo) from `mousePressed` + a small timer, keeping `UiInput` time-agnostic and testable.
- The milestone is larger than M62 (~8 tasks). If planning shows it's too big, split into M63a (Inventory + containers + 9-slice + styling, static screen) and M63b (drag/drop + double-click + scroll + demo). Default: keep as one.
