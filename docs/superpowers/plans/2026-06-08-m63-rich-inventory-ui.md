# M63 — Rich Inventory UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a single-player chest-looting screen — dual item grids with drag-and-drop + double-click transfer, scroll-on-overflow, and 9-slice chrome — on top of the M62 widget tree, plus a headless `Inventory` model reused by M64.

**Architecture:** Extend the existing headless `engine/ui/` widget system (`UiElement` tree → `UiLayout` → `UiInput` → `UiRender` → `HudBatch` → `drawHud`) with a Grid container, a ScrollBox with CPU quad-clipping, 9-slice image expansion, and drag/drop/double-click input. Add a pure `engine/game/Inventory` model. Demo in `games/13-loot`. No renderer changes; legacy UI untouched.

**Tech Stack:** C++17, Vulkan renderer (existing), `stb_truetype` (existing FontAtlas), nlohmann/json (existing serialize), GLFW input, custom `tests/test_framework.h`.

**Reference files (read before starting):**
- `engine/ui/UiElement.h` — widget struct + builders + `uiAssignIds`/`uiEqual`.
- `engine/ui/UiLayout.{h,cpp}` — `Rect`, `UiLayoutMap`, `layoutUi`, `resolveAnchored`, `layoutRec`.
- `engine/ui/UiInput.{h,cpp}` — `UiInputState`/`UiInputResult`/`updateUi`.
- `engine/ui/UiRender.{h,cpp}` — `appendQuad`/`appendText`/`groupFor`/`renderRec`/`renderUi`.
- `engine/ui/UiStack.{h,cpp}` — screen stack + focus carrying (`topFocus`/`setTopFocus`).
- `engine/ui/UiSerialize.cpp` — JSON field list.
- `engine/render/HudBatch.h` — `HudVertex{position,uv,color}`, `HudDrawGroup{texture,vertices}`, `HudBatch=vector<HudDrawGroup>`.
- `engine/render/Renderer.h` — `createTexture(w,h,rgba,srgb=true)`, `whiteTexture()`, `drawHud(batch,fbW,fbH)` (call BEFORE `endFrame`), `createMesh`, `createStandardLitShader`.
- `engine/core/Input.h/.cpp` — `mouseButtonDown/Pressed`, `mouseX/Y`; **no** released-edge or scroll yet (added in Task 7).
- `games/12-ui-arena/main.cpp` + `CMakeLists.txt` — the demo scaffold to model Task 8 on.

**Build/test commands (Windows, canonical build dir `build-vk`):**
- Configure (only if needed): `cmake --build build-vk --target <t>`
- Build a test: `cmake --build build-vk --target test_inventory`
- Run a test: `build-vk\tests\Debug\test_inventory.exe` (prints `OK - all checks passed` and returns 0 on success)
- Full suite is wired via `tests/CMakeLists.txt` (`iron_add_test`).

---

## File Structure

**Create:**
- `engine/game/Inventory.h` — item/stack/inventory model (declarations).
- `engine/game/Inventory.cpp` — model logic.
- `tests/test_inventory.cpp` — model tests.
- `games/13-loot/main.cpp` — demo.
- `games/13-loot/CMakeLists.txt` — demo build + asset copy.
- `games/13-loot/assets/ui/panel9.png`, `slot9.png` — 9-slice chrome (small bordered PNGs).
- `games/13-loot/assets/items/*.png` — ~6 item icons (or solid tiles).

**Modify:**
- `engine/ui/UiElement.h` — new kinds/fields/builders + `uiEqual`.
- `engine/ui/UiLayout.h` — `UiClipMap`, `applyScroll` decl.
- `engine/ui/UiLayout.cpp` — Grid branch + `applyScroll`.
- `engine/ui/UiRender.h` — `uiClipQuad` decl, `renderUi` clips param, `renderUiDragGhost` decl.
- `engine/ui/UiRender.cpp` — clipping, 9-slice, ghost.
- `engine/ui/UiInput.h` — `UiDragState`/`UiDropEvent`, extended state/result, `updateUi` params.
- `engine/ui/UiInput.cpp` — drag/drop/double-click logic.
- `engine/ui/UiStack.h/.cpp` — carry scroll offsets + drag + cursor; ghost in render.
- `engine/ui/UiSerialize.cpp` — new fields round-trip.
- `engine/core/Input.h/.cpp` — `mouseButtonReleased`, scroll wheel.
- `engine/CMakeLists.txt` — add `game/Inventory.cpp`.
- `tests/CMakeLists.txt` — add `test_inventory`.
- `CMakeLists.txt` — `add_subdirectory(games/13-loot)`.

**Note on `UiInputState`/`UiInputResult`:** append all new fields at the END of each struct so existing aggregate/positional initializers in `tests/test_ui.cpp` and `games/12-ui-arena/main.cpp` keep compiling.

---

## Task 1: Inventory model

**Files:**
- Create: `engine/game/Inventory.h`, `engine/game/Inventory.cpp`
- Test: `tests/test_inventory.cpp`
- Modify: `engine/CMakeLists.txt` (add `game/Inventory.cpp`), `tests/CMakeLists.txt` (add `test_inventory`)

- [ ] **Step 1: Write the failing test** — `tests/test_inventory.cpp`

```cpp
#include "game/Inventory.h"
#include "test_framework.h"

using namespace iron;

namespace {
// A tiny def table: id 1 = "potion" maxStack 5; id 2 = "sword" maxStack 1.
ItemDefTable makeDefs() {
    ItemDefTable t;
    t.add(ItemDef{1, "Potion", 5, kInvalidHandle});
    t.add(ItemDef{2, "Sword",  1, kInvalidHandle});
    return t;
}
}  // namespace

int main() {
    ItemDefTable defs = makeDefs();

    // addItem stacks up to maxStack, returns leftover.
    {
        Inventory inv(4);
        CHECK(inv.size() == 4);
        CHECK(inv.at(0).empty());
        const int leftover = inv.addItem(defs.get(1), 3);   // 3 potions
        CHECK(leftover == 0);
        CHECK(inv.at(0).item == 1u);
        CHECK(inv.at(0).count == 3);
        // Add 4 more: 2 top off slot 0 (to 5), 2 spill into slot 1.
        const int leftover2 = inv.addItem(defs.get(1), 4);
        CHECK(leftover2 == 0);
        CHECK(inv.at(0).count == 5);
        CHECK(inv.at(1).count == 2);
    }

    // addItem overflow returns leftover when inventory is full.
    {
        Inventory inv(1);
        const int leftover = inv.addItem(defs.get(1), 8);   // cap 5 in one slot
        CHECK(leftover == 3);
        CHECK(inv.at(0).count == 5);
    }

    // removeAt partial + whole.
    {
        Inventory inv(2);
        inv.addItem(defs.get(1), 5);
        CHECK(inv.removeAt(0, 2) == 2);
        CHECK(inv.at(0).count == 3);
        CHECK(inv.removeAt(0, 99) == 3);   // clamps to available
        CHECK(inv.at(0).empty());
    }

    // moveTo: place into empty.
    {
        Inventory a(2), b(2);
        a.addItem(defs.get(2), 1);                          // sword in a[0]
        CHECK(Inventory::moveTo(a, 0, b, 1, defs));
        CHECK(a.at(0).empty());
        CHECK(b.at(1).item == 2u);
    }

    // moveTo: merge same item up to maxStack, leftover stays at src.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(1), 4);
        b.addItem(defs.get(1), 3);                          // b[0] has 3
        CHECK(Inventory::moveTo(a, 0, b, 0, defs));         // b -> 5, a keeps 2
        CHECK(b.at(0).count == 5);
        CHECK(a.at(0).count == 2);
    }

    // moveTo: different items swap.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(2), 1);                          // sword
        b.addItem(defs.get(1), 2);                          // potions
        CHECK(Inventory::moveTo(a, 0, b, 0, defs));
        CHECK(a.at(0).item == 1u && a.at(0).count == 2);
        CHECK(b.at(0).item == 2u && b.at(0).count == 1);
    }

    // quickTransfer: into first mergeable/free slot of dst.
    {
        Inventory a(1), b(2);
        a.addItem(defs.get(1), 3);
        b.addItem(defs.get(1), 4);                          // b[0]=4 (mergeable)
        CHECK(Inventory::quickTransfer(a, 0, b, defs));
        CHECK(b.at(0).count == 5);                          // topped off
        CHECK(b.at(1).count == 2);                          // spill
        CHECK(a.at(0).empty());
    }

    // quickTransfer: no room -> returns false, src unchanged.
    {
        Inventory a(1), b(1);
        a.addItem(defs.get(2), 1);                          // sword (non-stack)
        b.addItem(defs.get(1), 5);                          // b full, different item
        CHECK(!Inventory::quickTransfer(a, 0, b, defs));
        CHECK(a.at(0).item == 2u);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run test to verify it fails to build**

Run: `cmake --build build-vk --target test_inventory`
Expected: FAIL — `Inventory.h` / `test_inventory` target not found.

- [ ] **Step 3: Write `engine/game/Inventory.h`**

```cpp
#pragma once

#include "render/Handles.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

using ItemId = std::uint32_t;          // 0 = none/empty
constexpr ItemId kNoItem = 0;

// Static, game-owned description of an item kind.
struct ItemDef {
    ItemId        id      = kNoItem;
    std::string   name;
    int           maxStack = 1;        // 1 = non-stackable
    TextureHandle icon    = kInvalidHandle;
};

// Lookup of ItemDefs by id (game owns it; the model reads maxStack during merges).
class ItemDefTable {
public:
    void add(const ItemDef& d) { defs_[d.id] = d; }
    const ItemDef& get(ItemId id) const {
        const auto it = defs_.find(id);
        return it != defs_.end() ? it->second : none_;
    }
private:
    std::unordered_map<ItemId, ItemDef> defs_;
    ItemDef none_{};                   // id 0, maxStack 1
};

// One slot's contents. count == 0 (or item == kNoItem) means empty.
struct ItemStack {
    ItemId item  = kNoItem;
    int    count = 0;
    bool empty() const { return item == kNoItem || count <= 0; }
};

// A fixed-size array of slots.
class Inventory {
public:
    explicit Inventory(int slotCount) : slots_(slotCount > 0 ? slotCount : 0) {}

    int size() const { return static_cast<int>(slots_.size()); }
    const ItemStack& at(int slot) const { return slots_.at(static_cast<std::size_t>(slot)); }

    // Add `count` of `def` into mergeable then free slots; returns leftover (0 = all placed).
    int addItem(const ItemDef& def, int count);
    // Remove up to `count` from `slot`; returns the number actually removed.
    int removeAt(int slot, int count);

    // Move the stack at (src,srcSlot) onto (dst,dstSlot): empty=place, same=merge
    // (leftover stays at src), different=swap. Returns true if anything changed.
    static bool moveTo(Inventory& src, int srcSlot,
                       Inventory& dst, int dstSlot, const ItemDefTable& defs);
    // Auto-move the stack at (src,srcSlot) into dst's first mergeable/free slots.
    // Returns true if anything moved.
    static bool quickTransfer(Inventory& src, int srcSlot,
                              Inventory& dst, const ItemDefTable& defs);

private:
    std::vector<ItemStack> slots_;
};

}  // namespace iron
```

- [ ] **Step 4: Write `engine/game/Inventory.cpp`**

```cpp
#include "game/Inventory.h"

#include <algorithm>

namespace iron {

int Inventory::addItem(const ItemDef& def, int count) {
    if (def.id == kNoItem || count <= 0) return count;
    const int maxStack = def.maxStack > 0 ? def.maxStack : 1;
    // First pass: top off existing stacks of the same item.
    for (ItemStack& s : slots_) {
        if (count <= 0) break;
        if (s.item == def.id && s.count < maxStack) {
            const int room = maxStack - s.count;
            const int put = std::min(room, count);
            s.count += put;
            count -= put;
        }
    }
    // Second pass: fill empty slots.
    for (ItemStack& s : slots_) {
        if (count <= 0) break;
        if (s.empty()) {
            const int put = std::min(maxStack, count);
            s.item = def.id;
            s.count = put;
            count -= put;
        }
    }
    return count;  // leftover
}

int Inventory::removeAt(int slot, int count) {
    if (slot < 0 || slot >= size() || count <= 0) return 0;
    ItemStack& s = slots_[static_cast<std::size_t>(slot)];
    const int removed = std::min(count, s.count);
    s.count -= removed;
    if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
    return removed;
}

bool Inventory::moveTo(Inventory& src, int srcSlot,
                       Inventory& dst, int dstSlot, const ItemDefTable& defs) {
    if (srcSlot < 0 || srcSlot >= src.size() ||
        dstSlot < 0 || dstSlot >= dst.size()) return false;
    ItemStack& s = src.slots_[static_cast<std::size_t>(srcSlot)];
    ItemStack& d = dst.slots_[static_cast<std::size_t>(dstSlot)];
    if (s.empty()) return false;
    if (&s == &d) return false;

    if (d.empty()) {                                   // place
        d = s;
        s.item = kNoItem; s.count = 0;
        return true;
    }
    if (d.item == s.item) {                            // merge
        const int maxStack = defs.get(d.item).maxStack > 0 ? defs.get(d.item).maxStack : 1;
        const int room = maxStack - d.count;
        if (room <= 0) return false;
        const int moved = std::min(room, s.count);
        d.count += moved;
        s.count -= moved;
        if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
        return moved > 0;
    }
    std::swap(s, d);                                   // different -> swap
    return true;
}

bool Inventory::quickTransfer(Inventory& src, int srcSlot,
                              Inventory& dst, const ItemDefTable& defs) {
    if (srcSlot < 0 || srcSlot >= src.size()) return false;
    ItemStack& s = src.slots_[static_cast<std::size_t>(srcSlot)];
    if (s.empty()) return false;
    const int before = s.count;
    const int leftover = dst.addItem(defs.get(s.item), s.count);
    s.count = leftover;
    if (s.count <= 0) { s.item = kNoItem; s.count = 0; }
    return leftover < before;
}

}  // namespace iron
```

- [ ] **Step 5: Register in CMake**

In `engine/CMakeLists.txt`, in the source list near `game/Collision.cpp`, add:
```cmake
  game/Inventory.cpp
```
In `tests/CMakeLists.txt`, after `iron_add_test(test_collision ...)` (or near the other game tests), add:
```cmake
iron_add_test(test_inventory test_inventory.cpp)
```

- [ ] **Step 6: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_inventory` then `build-vk\tests\Debug\test_inventory.exe`
Expected: PASS — `OK - all checks passed`.

- [ ] **Step 7: Commit**

```bash
git add engine/game/Inventory.h engine/game/Inventory.cpp tests/test_inventory.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M63: Inventory model (stacks, add/remove/move/quickTransfer) + tests"
```

---

## Task 2: UiElement extensions (kinds, fields, builders, equality, serialize)

**Files:**
- Modify: `engine/ui/UiElement.h`, `engine/ui/UiSerialize.cpp`
- Test: `tests/test_ui.cpp` (append new cases at end of `main`, before `return iron_test_result();`)

- [ ] **Step 1: Write the failing test** — append to `tests/test_ui.cpp`

```cpp
    // M63: new kinds, slot flags, and serialize round-trip for new fields.
    {
        UiElement grid = uiGrid(Anchor::TopLeft, Vec2{10, 10}, Vec2{200, 120}, 4, 6.0f);
        CHECK(grid.kind == UiKind::Grid);
        CHECK(grid.gridCols == 4);
        CHECK(grid.spacing == 6.0f);

        UiElement scroll = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{180, 100}, 4.0f);
        CHECK(scroll.kind == UiKind::ScrollBox);

        UiElement slot = uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                kInvalidHandle, Vec4{8, 8, 8, 8}, 0.25f,
                                /*userData=*/0x00010003u);
        CHECK(slot.kind == UiKind::Image);
        CHECK(slot.draggable);
        CHECK(slot.dropTarget);
        CHECK(slot.userData == 0x00010003u);
        CHECK(slot.nineSliceUv == 0.25f);
        CHECK(slot.nineSliceMargin.x == 8.0f);

        // Serialize round-trips the new fields (texture/id still excluded).
        const nlohmann::json j = uiToJson(slot);
        const UiElement back = uiFromJson(j);
        CHECK(uiEqual(slot, back));
        // uiEqual is sensitive to the new fields.
        UiElement diff = slot; diff.userData = 99u;
        CHECK(!uiEqual(slot, diff));
        UiElement diff2 = slot; diff2.gridCols = 7;
        CHECK(!uiEqual(slot, diff2));
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_ui`
Expected: FAIL — `uiGrid`/`uiScrollBox`/`uiSlot`/`UiKind::Grid` undeclared.

- [ ] **Step 3: Extend `engine/ui/UiElement.h`**

Change the enum:
```cpp
enum class UiKind { Panel, Label, Image, Button, Bar, Grid, ScrollBox };
```
Add fields to `struct UiElement` (after `actionId`, before `children`):
```cpp
    int           gridCols = 0;                // Grid: columns per row
    float         scrollOffset = 0.0f;         // ScrollBox: current vertical scroll (px)
    Vec4          nineSliceMargin{0, 0, 0, 0}; // L,T,R,B dest border px (0 => plain quad)
    float         nineSliceUv = 0.0f;          // source border fraction 0..1 (uniform)
    bool          draggable = false;           // can initiate a drag
    bool          dropTarget = false;          // can receive a drop
    std::uint32_t userData = 0;                // payload (game encodes container/slot)
```
Add builders (after `uiBar`):
```cpp
inline UiElement uiGrid(Anchor a, Vec2 offset, Vec2 size, int cols, float spacing) {
    UiElement e = uiPanel(a, offset, size, Vec4{0, 0, 0, 0});
    e.kind = UiKind::Grid; e.gridCols = cols; e.spacing = spacing; return e;
}
inline UiElement uiScrollBox(Anchor a, Vec2 offset, Vec2 size, float spacing) {
    UiElement e = uiPanel(a, offset, size, Vec4{0, 0, 0, 0});
    e.kind = UiKind::ScrollBox; e.spacing = spacing; return e;
}
inline UiElement uiImage9(Anchor a, Vec2 offset, Vec2 size, TextureHandle tex,
                          Vec4 margin, float uvBorder, Vec4 tint) {
    UiElement e = uiImage(a, offset, size, tex, tint);
    e.nineSliceMargin = margin; e.nineSliceUv = uvBorder; return e;
}
// A draggable/droppable 9-slice tile, ready to receive icon + count children.
inline UiElement uiSlot(Anchor a, Vec2 offset, Vec2 size, TextureHandle tileTex,
                        Vec4 margin, float uvBorder, std::uint32_t userData,
                        Vec4 tint = Vec4{1, 1, 1, 1}) {
    UiElement e = uiImage9(a, offset, size, tileTex, margin, uvBorder, tint);
    e.draggable = true; e.dropTarget = true; e.userData = userData; return e;
}
```
Extend `uiEqual` — add these comparisons inside the function (before the children loop):
```cpp
    if (a.gridCols != b.gridCols) return false;
    if (!nf(a.scrollOffset, b.scrollOffset)) return false;
    if (!n4(a.nineSliceMargin, b.nineSliceMargin) || !nf(a.nineSliceUv, b.nineSliceUv)) return false;
    if (a.draggable != b.draggable || a.dropTarget != b.dropTarget) return false;
    if (a.userData != b.userData) return false;
```

- [ ] **Step 4: Extend `engine/ui/UiSerialize.cpp`**

In `uiToJson`, after `j["actionId"] = e.actionId;`:
```cpp
    j["gridCols"]        = e.gridCols;
    j["scrollOffset"]    = e.scrollOffset;
    j["nineSliceMargin"] = v4(e.nineSliceMargin);
    j["nineSliceUv"]     = e.nineSliceUv;
    j["draggable"]       = e.draggable;
    j["dropTarget"]      = e.dropTarget;
    j["userData"]        = e.userData;
```
In `uiFromJson`, after `e.actionId = j.value("actionId", 0u);`:
```cpp
    e.gridCols        = j.value("gridCols", 0);
    e.scrollOffset    = j.value("scrollOffset", 0.0f);
    e.nineSliceMargin = readV4(j.contains("nineSliceMargin") ? j["nineSliceMargin"] : nlohmann::json{}, Vec4{0, 0, 0, 0});
    e.nineSliceUv     = j.value("nineSliceUv", 0.0f);
    e.draggable       = j.value("draggable", false);
    e.dropTarget      = j.value("dropTarget", false);
    e.userData        = j.value("userData", 0u);
```

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_ui` then `build-vk\tests\Debug\test_ui.exe`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiElement.h engine/ui/UiSerialize.cpp tests/test_ui.cpp
git commit -m "M63: UiElement Grid/ScrollBox kinds + 9-slice/slot fields + builders + serialize"
```

---

## Task 3: UiLayout — Grid layout + scroll/clip pass

**Files:**
- Modify: `engine/ui/UiLayout.h`, `engine/ui/UiLayout.cpp`
- Test: `tests/test_ui.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/test_ui.cpp`

```cpp
    // M63: Grid wraps children into rows of `gridCols` using child size + spacing.
    {
        UiElement grid = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200}, 3, 10.0f);
        for (int i = 0; i < 5; ++i)
            grid.children.push_back(uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                            Vec4{1, 1, 1, 1}));
        uiAssignIds(grid);
        const UiLayoutMap m = layoutUi(grid, Vec2{300, 300});
        // child 0 at (0,0); child 1 at (50,0); child 2 at (100,0); child 3 wraps to (0,50).
        CHECK(m.at(grid.children[0].id).min.x == 0.0f);
        CHECK(m.at(grid.children[1].id).min.x == 50.0f);
        CHECK(m.at(grid.children[2].id).min.x == 100.0f);
        CHECK(m.at(grid.children[3].id).min.x == 0.0f);
        CHECK(m.at(grid.children[3].id).min.y == 50.0f);
    }

    // M63: applyScroll shifts ScrollBox descendants up by the offset and clips them.
    {
        UiElement box = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, 0.0f);
        UiElement inner = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 300}, 1, 0.0f);
        for (int i = 0; i < 6; ++i)                          // 6 * 50 = 300 tall content
            inner.children.push_back(uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                             Vec4{1, 1, 1, 1}));
        box.children.push_back(std::move(inner));
        uiAssignIds(box);
        UiLayoutMap m = layoutUi(box, Vec2{200, 400});
        const UiId child0 = box.children[0].children[0].id;
        const float before = m.at(child0).min.y;
        std::unordered_map<UiId, float> offsets{{box.id, 40.0f}};
        const UiClipMap clips = applyScroll(box, m, offsets);
        CHECK(m.at(child0).min.y == before - 40.0f);         // shifted up
        CHECK(clips.count(child0) == 1u);                    // descendant is clipped
        CHECK(clips.at(child0).min.y == 0.0f);               // clip == scrollbox rect
        CHECK(clips.at(child0).max.y == 100.0f);
        // Offset clamps to [0, contentHeight - viewport] = [0, 200].
        std::unordered_map<UiId, float> tooBig{{box.id, 9999.0f}};
        UiLayoutMap m2 = layoutUi(box, Vec2{200, 400});
        applyScroll(box, m2, tooBig);
        const UiId last = box.children[0].children[5].id;
        // content bottom (originally 300) shifted by clamped 200 -> 100 == viewport bottom.
        CHECK(m2.at(last).max.y == 100.0f);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_ui`
Expected: FAIL — `applyScroll`/`UiClipMap` undeclared; Grid not laid out (child 1 min.x == 0, not 50).

- [ ] **Step 3: Extend `engine/ui/UiLayout.h`**

After the `using UiLayoutMap` line:
```cpp
// Clip rects for elements inside a scrolled ScrollBox (element id -> viewport rect).
using UiClipMap = std::unordered_map<UiId, Rect>;
```
After the `layoutUi` declaration:
```cpp
// Post-layout pass: for each ScrollBox in `root` with a non-zero offset in
// `offsets` (keyed by ScrollBox id), shift all its descendant rects up by the
// (clamped) offset and assign them a clip rect equal to the ScrollBox's rect.
// Mutates `rects` in place; returns the clip map. The offset is clamped to
// [0, contentHeight - viewportHeight] so stale/oversized values self-correct.
UiClipMap applyScroll(const UiElement& root, UiLayoutMap& rects,
                      const std::unordered_map<UiId, float>& offsets);
```

- [ ] **Step 4: Extend `engine/ui/UiLayout.cpp`**

In `layoutRec`, add a Grid branch. Replace the `if (e.stack == StackDir::None) { ... }` opening so Grid is handled first:
```cpp
void layoutRec(const UiElement& e, const Rect& eRect, UiLayoutMap& out) {
    if (e.id != 0) out[e.id] = eRect;

    if (e.kind == UiKind::Grid && e.gridCols > 0) {
        float x = eRect.min.x, y = eRect.min.y, rowH = 0.0f;
        int col = 0;
        for (const UiElement& c : e.children) {
            const Rect cr{Vec2{x, y}, Vec2{x + c.size.x, y + c.size.y}};
            layoutRec(c, cr, out);
            rowH = std::max(rowH, c.size.y);
            x += c.size.x + e.spacing;
            if (++col >= e.gridCols) {                    // wrap to next row
                col = 0; x = eRect.min.x; y += rowH + e.spacing; rowH = 0.0f;
            }
        }
        return;
    }

    if (e.stack == StackDir::None) {
        for (const UiElement& c : e.children)
            layoutRec(c, resolveAnchored(eRect, c.anchor, c.size, c.offset), out);
        return;
    }
    // ... existing Stack code unchanged ...
}
```
Add `#include <algorithm>` at the top for `std::max`.

Add `applyScroll` at the bottom of the `iron` namespace (after `layoutUi`):
```cpp
namespace {

// Collect ids of e and all descendants.
void collectSubtreeIds(const UiElement& e, std::vector<UiId>& out) {
    if (e.id != 0) out.push_back(e.id);
    for (const UiElement& c : e.children) collectSubtreeIds(c, out);
}

void applyScrollRec(const UiElement& e, UiLayoutMap& rects,
                    const std::unordered_map<UiId, float>& offsets, UiClipMap& clips) {
    if (e.kind == UiKind::ScrollBox) {
        const auto boxIt = rects.find(e.id);
        const auto offIt = offsets.find(e.id);
        if (boxIt != rects.end()) {
            const Rect viewport = boxIt->second;
            const float viewH = viewport.max.y - viewport.min.y;
            // Content height = (max descendant bottom) - viewport top, pre-shift.
            std::vector<UiId> ids;
            for (const UiElement& c : e.children) collectSubtreeIds(c, ids);
            float contentBottom = viewport.min.y;
            for (UiId id : ids) {
                const auto it = rects.find(id);
                if (it != rects.end()) contentBottom = std::max(contentBottom, it->second.max.y);
            }
            const float contentH = contentBottom - viewport.min.y;
            float off = (offIt != offsets.end()) ? offIt->second : 0.0f;
            const float maxOff = std::max(0.0f, contentH - viewH);
            if (off < 0.0f) off = 0.0f;
            if (off > maxOff) off = maxOff;
            for (UiId id : ids) {
                const auto it = rects.find(id);
                if (it == rects.end()) continue;
                it->second.min.y -= off;
                it->second.max.y -= off;
                clips[id] = viewport;
            }
        }
    }
    for (const UiElement& c : e.children) applyScrollRec(c, rects, offsets, clips);
}

}  // namespace

UiClipMap applyScroll(const UiElement& root, UiLayoutMap& rects,
                      const std::unordered_map<UiId, float>& offsets) {
    UiClipMap clips;
    applyScrollRec(root, rects, offsets, clips);
    return clips;
}
```
(Move the existing anonymous-namespace block's closing/opening as needed so these helpers compile; simplest is to add a second `namespace { ... }` block after `layoutUi` as shown.)

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_ui` then `build-vk\tests\Debug\test_ui.exe`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiLayout.h engine/ui/UiLayout.cpp tests/test_ui.cpp
git commit -m "M63: UiLayout Grid wrap + applyScroll clip/offset pass + tests"
```

---

## Task 4: UiRender — quad clipping, 9-slice expansion, drag ghost

**Files:**
- Modify: `engine/ui/UiRender.h`, `engine/ui/UiRender.cpp`
- Test: `tests/test_ui.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/test_ui.cpp`

```cpp
    // M63: uiClipQuad intersects a quad and remaps UVs proportionally.
    {
        Vec2 mn{0, 0}, mx{100, 100}, uv0{0, 0}, uv1{1, 1};
        const Rect clip{Vec2{50, 0}, Vec2{200, 200}};       // clip left half away
        const bool kept = uiClipQuad(mn, mx, uv0, uv1, clip);
        CHECK(kept);
        CHECK(mn.x == 50.0f);
        CHECK(uv0.x == 0.5f);                                // uv remapped to half
        CHECK(mx.x == 100.0f);
        // Fully outside -> dropped.
        Vec2 a{0, 0}, b{10, 10}, c{0, 0}, d{1, 1};
        CHECK(!uiClipQuad(a, b, c, d, Rect{Vec2{500, 500}, Vec2{600, 600}}));
    }

    // M63: a 9-slice image emits 9 quads (54 verts) into its texture group.
    {
        FontAtlas dummy;                                     // no texture; not used here
        UiElement panel = uiImage9(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100},
                                   /*tex=*/7, Vec4{10, 10, 10, 10}, 0.25f, Vec4{1, 1, 1, 1});
        uiAssignIds(panel);
        const UiLayoutMap m = layoutUi(panel, Vec2{200, 200});
        const HudBatch b = renderUi(panel, m, dummy, /*white=*/1, 0, 0);
        std::size_t verts = 0;
        for (const auto& g : b) if (g.texture == 7) verts += g.vertices.size();
        CHECK(verts == 54u);                                 // 9 quads * 6 verts
    }

    // M63: clips param trims a panel's quad to the clip rect.
    {
        FontAtlas dummy;
        UiElement p = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, Vec4{1, 1, 1, 1});
        uiAssignIds(p);
        const UiLayoutMap m = layoutUi(p, Vec2{200, 200});
        UiClipMap clips; clips[p.id] = Rect{Vec2{0, 0}, Vec2{100, 40}};
        const HudBatch b = renderUi(p, m, dummy, /*white=*/1, 0, 0, clips);
        // The single quad's lowest y must be clamped to 40.
        float maxY = 0.0f;
        for (const auto& g : b) for (const auto& v : g.vertices) maxY = std::max(maxY, v.position.y);
        CHECK(maxY == 40.0f);
    }

    // M63: drag ghost re-renders the dragged subtree translated to the cursor.
    {
        FontAtlas dummy;
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                       /*tile=*/0, Vec4{0, 0, 0, 0}, 0.0f, 0x10001u));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{200, 200});
        UiDragState drag; drag.active = true; drag.sourceId = root.children[0].id;
        drag.grabOffset = Vec2{0, 0};
        const HudBatch ghost = renderUiDragGhost(root, m, dummy, /*white=*/1, drag, Vec2{120, 130});
        bool any = false;
        for (const auto& g : ghost) if (!g.vertices.empty()) any = true;
        CHECK(any);                                          // ghost emitted something
        // Ghost geometry sits near the cursor (x >= ~120), not at the origin slot.
        float minX = 1e9f;
        for (const auto& g : ghost) for (const auto& v : g.vertices) minX = std::min(minX, v.position.x);
        CHECK(minX >= 120.0f - 0.01f);
    }
```
(Add `#include "ui/UiInput.h"` to `test_ui.cpp` if not present — it is, per the existing includes. `UiDragState` lives there after Task 5; **reorder execution so Task 5 lands before building this test, or** declare the ghost test after Task 5. To keep tasks independently buildable, place the ghost test block in Task 5's test step instead and keep Task 4's tests to clip + 9-slice + clip-param. **Do that:** move the "drag ghost" CHECK block to Task 5.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_ui`
Expected: FAIL — `uiClipQuad` undeclared; 9-slice path missing (verts != 54).

- [ ] **Step 3: Extend `engine/ui/UiRender.h`**

```cpp
#include "ui/UiInput.h"   // for UiDragState (defined in Task 5)
```
After the includes, before `renderUi`:
```cpp
// Clip an axis-aligned quad to `clip`, remapping UVs proportionally. Mutates the
// passed min/max/uv in place. Returns false if the quad is fully outside.
bool uiClipQuad(Vec2& min, Vec2& max, Vec2& uvMin, Vec2& uvMax, const Rect& clip);
```
Change `renderUi` to take an optional clip map:
```cpp
HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused, const UiClipMap& clips = {});
```
Add after `renderUi`:
```cpp
// Render the dragged element's subtree translated so its top-left follows
// (cursor - drag.grabOffset), semi-transparent, as an on-top overlay. Returns an
// empty batch if !drag.active or the source id isn't in `rects`.
HudBatch renderUiDragGhost(const UiElement& root, const UiLayoutMap& rects,
                           const FontAtlas& atlas, TextureHandle whiteTexture,
                           const UiDragState& drag, Vec2 cursor);
```

- [ ] **Step 4: Extend `engine/ui/UiRender.cpp`**

Add `#include <algorithm>` at top. Add the clip helper in the anonymous namespace:
```cpp
}  // (close existing anon namespace? no — add inside it, before renderRec)
```
Concretely, inside the existing `namespace { ... }`, add:
```cpp
bool clipQuadImpl(Vec2& mn, Vec2& mx, Vec2& uvMin, Vec2& uvMax, const Rect& clip) {
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    if (w <= 0.0f || h <= 0.0f) return false;
    const float nx0 = std::max(mn.x, clip.min.x);
    const float ny0 = std::max(mn.y, clip.min.y);
    const float nx1 = std::min(mx.x, clip.max.x);
    const float ny1 = std::min(mx.y, clip.max.y);
    if (nx1 <= nx0 || ny1 <= ny0) return false;
    const float uw = uvMax.x - uvMin.x, uh = uvMax.y - uvMin.y;
    const Vec2 nUvMin{uvMin.x + (nx0 - mn.x) / w * uw, uvMin.y + (ny0 - mn.y) / h * uh};
    const Vec2 nUvMax{uvMin.x + (nx1 - mn.x) / w * uw, uvMin.y + (ny1 - mn.y) / h * uh};
    mn = Vec2{nx0, ny0}; mx = Vec2{nx1, ny1};
    uvMin = nUvMin; uvMax = nUvMax;
    return true;
}

// appendQuad that clips first when `clip` is non-null.
void appendQuadClipped(std::vector<HudVertex>& out, Vec2 mn, Vec2 mx,
                       Vec2 uvMin, Vec2 uvMax, Vec4 color, const Rect* clip) {
    if (clip) { if (!clipQuadImpl(mn, mx, uvMin, uvMax, *clip)) return; }
    appendQuad(out, mn, mx, uvMin, uvMax, color);
}

// Emit a 9-slice expansion of [r] using margin (L,T,R,B px) and uniform source
// border fraction `uvb`, into `verts`, clipped by `clip` if non-null.
void appendNineSlice(std::vector<HudVertex>& verts, const Rect& r, Vec4 m,
                     float uvb, Vec4 color, const Rect* clip) {
    const float xs[4] = {r.min.x, r.min.x + m.x, r.max.x - m.z, r.max.x};
    const float ys[4] = {r.min.y, r.min.y + m.y, r.max.y - m.w, r.max.y};
    const float us[4] = {0.0f, uvb, 1.0f - uvb, 1.0f};
    const float vs[4] = {0.0f, uvb, 1.0f - uvb, 1.0f};
    for (int row = 0; row < 3; ++row) {
        for (int colc = 0; colc < 3; ++colc) {
            const Vec2 mn{xs[colc], ys[row]}, mx{xs[colc + 1], ys[row + 1]};
            if (mx.x <= mn.x || mx.y <= mn.y) continue;     // degenerate cell
            appendQuadClipped(verts, mn, mx, Vec2{us[colc], vs[row]},
                              Vec2{us[colc + 1], vs[row + 1]}, color, clip);
        }
    }
}
```
Update `appendText` to take a clip and clip each glyph quad:
```cpp
void appendText(HudBatch& batch, const FontAtlas& atlas, const std::string& text,
                Vec2 topLeft, float fontPx, Vec4 color, const Rect* clip) {
    if (atlas.texture == kInvalidHandle || atlas.pixelHeight() <= 0.0f) return;
    const float scale = fontPx / atlas.pixelHeight();
    const Vec2 origin{topLeft.x, topLeft.y + fontPx * 0.8f};
    std::vector<HudVertex>& verts = groupFor(batch, atlas.texture);
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) {
        const GlyphQuad q = atlas.quadFor(c, penX, penY);
        if (q.min.x == q.max.x || q.min.y == q.max.y) continue;
        appendQuadClipped(verts, origin + q.min * scale, origin + q.max * scale,
                          q.uvMin, q.uvMax, color, clip);
    }
}
```
Update `renderRec` to look up the clip and route every emit through the clipped variants. At the top of `renderRec`, after `const Rect r = it->second;` add:
```cpp
    const auto clipIt = clips.find(e.id);
    const Rect* clip = (clipIt != clips.end()) ? &clipIt->second : nullptr;
```
Then in each case replace bare `appendQuad(groupFor(...), ...)` with `appendQuadClipped(groupFor(...), ..., clip)` and `appendText(batch, atlas, ..., color)` with `appendText(batch, atlas, ..., clip)`. For the **Image** case, branch on 9-slice:
```cpp
        case UiKind::Image:
            if (e.nineSliceMargin.x != 0.0f || e.nineSliceMargin.y != 0.0f ||
                e.nineSliceMargin.z != 0.0f || e.nineSliceMargin.w != 0.0f)
                appendNineSlice(groupFor(batch, e.texture), r, e.nineSliceMargin,
                                e.nineSliceUv, e.color, clip);
            else
                appendQuadClipped(groupFor(batch, e.texture), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
```
`renderRec` must take `const UiClipMap& clips` — thread it through the signature and the recursive call. Add a `Grid`/`ScrollBox` case: both are containers — render their background like a Panel (only if `color.w > 0`) then recurse (the existing `default`/Panel path already covers a transparent container; add explicit cases that fall through to the Panel quad emit, or just let them hit a Panel-like branch). Simplest: add
```cpp
        case UiKind::Grid:
        case UiKind::ScrollBox:
            if (e.color.w > 0.0f)
                appendQuadClipped(groupFor(batch, white), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
```
Update `renderUi` to pass `clips` into `renderRec`:
```cpp
HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused, const UiClipMap& clips) {
    HudBatch batch;
    renderRec(root, rects, atlas, whiteTexture, hovered, focused, clips, batch);
    return batch;
}
```
Add the exported clip helper + the ghost function (outside the anon namespace):
```cpp
bool uiClipQuad(Vec2& min, Vec2& max, Vec2& uvMin, Vec2& uvMax, const Rect& clip) {
    return clipQuadImpl(min, max, uvMin, uvMax, clip);
}

namespace {
// Find element by id (depth-first).
const UiElement* findById(const UiElement& e, UiId id) {
    if (e.id == id) return &e;
    for (const UiElement& c : e.children) if (const UiElement* f = findById(c, id)) return f;
    return nullptr;
}
// Re-render a subtree with all rects translated by `delta`, alpha-scaled.
void renderGhostRec(const UiElement& e, const UiLayoutMap& rects, const FontAtlas& atlas,
                    TextureHandle white, Vec2 delta, float alpha, HudBatch& batch) {
    if (!e.visible) return;
    const auto it = rects.find(e.id);
    if (it != rects.end()) {
        const Rect r{it->second.min + delta, it->second.max + delta};
        Vec4 col = e.color; col.w *= alpha;
        if (e.kind == UiKind::Image) {
            if (e.nineSliceMargin.x != 0.0f || e.nineSliceMargin.y != 0.0f ||
                e.nineSliceMargin.z != 0.0f || e.nineSliceMargin.w != 0.0f)
                appendNineSlice(groupFor(batch, e.texture), r, e.nineSliceMargin,
                                e.nineSliceUv, col, nullptr);
            else
                appendQuadClipped(groupFor(batch, e.texture), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, col, nullptr);
        } else if (e.kind == UiKind::Label) {
            appendText(batch, atlas, e.text, r.min, e.fontPx, col, nullptr);
        } else if (e.color.w > 0.0f) {
            appendQuadClipped(groupFor(batch, white), r.min, r.max,
                              Vec2{0, 0}, Vec2{1, 1}, col, nullptr);
        }
    }
    for (const UiElement& c : e.children)
        renderGhostRec(c, rects, atlas, white, delta, alpha, batch);
}
}  // namespace

HudBatch renderUiDragGhost(const UiElement& root, const UiLayoutMap& rects,
                           const FontAtlas& atlas, TextureHandle whiteTexture,
                           const UiDragState& drag, Vec2 cursor) {
    HudBatch batch;
    if (!drag.active) return batch;
    const UiElement* src = findById(root, drag.sourceId);
    if (!src) return batch;
    const auto it = rects.find(drag.sourceId);
    if (it == rects.end()) return batch;
    const Vec2 newMin{cursor.x - drag.grabOffset.x, cursor.y - drag.grabOffset.y};
    const Vec2 delta{newMin.x - it->second.min.x, newMin.y - it->second.min.y};
    renderGhostRec(*src, rects, atlas, whiteTexture, delta, 0.75f, batch);
    return batch;
}
```
(If `Vec2`/`Vec4` lack `operator+`, the M62 code already uses `origin + q.min * scale`, so `+` and scalar `*` exist; `it->second.min + delta` is fine.)

- [ ] **Step 5: Build + run to verify it passes** (the ghost test block lives in Task 5)

Run: `cmake --build build-vk --target test_ui` then `build-vk\tests\Debug\test_ui.exe`
Expected: PASS for clip + 9-slice + clip-param cases.

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiRender.h engine/ui/UiRender.cpp tests/test_ui.cpp
git commit -m "M63: UiRender quad clipping + 9-slice expansion + drag-ghost overlay + tests"
```

---

## Task 5: UiInput — drag/drop + double-click + clip-aware hit-testing

**Files:**
- Modify: `engine/ui/UiInput.h`, `engine/ui/UiInput.cpp`
- Test: `tests/test_ui.cpp` (append; include the ghost test from Task 4 here too)

- [ ] **Step 1: Write the failing test** — append to `tests/test_ui.cpp`

```cpp
    // M63: drag start on press over a draggable; release over a dropTarget emits a drop.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{300, 100}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0},   Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00000001u));  // src slot
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{100, 0}, Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00010002u));  // dst slot
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{300, 100});

        // Frame 1: press on src slot -> drag becomes active.
        UiInputState s1; s1.mouse = Vec2{20, 20}; s1.mousePressed = true; s1.mouseDown = true;
        UiInputResult r1 = updateUi(root, m, s1, 0, UiDragState{}, UiClipMap{});
        CHECK(r1.drag.active);
        CHECK(r1.drag.sourceUserData == 0x00000001u);

        // Frame 2: release over dst slot -> drop{src,dst}, drag clears.
        UiInputState s2; s2.mouse = Vec2{120, 20}; s2.mouseReleased = true;
        UiInputResult r2 = updateUi(root, m, s2, 0, r1.drag, UiClipMap{});
        CHECK(r2.drop.has_value());
        CHECK(r2.drop->source == 0x00000001u);
        CHECK(r2.drop->target == 0x00010002u);
        CHECK(!r2.drag.active);
    }

    // M63: double-click on a draggable emits quickTransfer, no drag.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00000005u));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{100, 100});
        UiInputState s; s.mouse = Vec2{20, 20}; s.mousePressed = true; s.mouseDown = true;
        s.doubleClick = true;
        UiInputResult r = updateUi(root, m, s, 0, UiDragState{}, UiClipMap{});
        CHECK(r.quickTransfer.has_value());
        CHECK(*r.quickTransfer == 0x00000005u);
        CHECK(!r.drag.active);
    }

    // M63: wheel over a scrollbox produces a scroll delta keyed by its id.
    {
        UiElement box = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, 0.0f);
        uiAssignIds(box);
        const UiLayoutMap m = layoutUi(box, Vec2{200, 200});
        UiInputState s; s.mouse = Vec2{50, 50}; s.wheel = -1.0f;   // scroll down
        UiInputResult r = updateUi(box, m, s, 0, UiDragState{}, UiClipMap{});
        CHECK(r.scrollDeltas.size() == 1u);
        CHECK(r.scrollDeltas[0].first == box.id);
        CHECK(r.scrollDeltas[0].second > 0.0f);               // down -> increase offset
    }

    // (Ghost test moved from Task 4.)
    {
        FontAtlas dummy;
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                       0, Vec4{0, 0, 0, 0}, 0.0f, 0x10001u));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{200, 200});
        UiDragState drag; drag.active = true; drag.sourceId = root.children[0].id;
        drag.grabOffset = Vec2{0, 0};
        const HudBatch ghost = renderUiDragGhost(root, m, dummy, 1, drag, Vec2{120, 130});
        float minX = 1e9f; bool any = false;
        for (const auto& g : ghost) for (const auto& v : g.vertices) { any = true; minX = std::min(minX, v.position.x); }
        CHECK(any);
        CHECK(minX >= 120.0f - 0.01f);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_ui`
Expected: FAIL — `UiDragState`/`UiDropEvent` undeclared; `updateUi` arity mismatch.

- [ ] **Step 3: Extend `engine/ui/UiInput.h`**

Add includes: `#include <optional>` and `#include <utility>`. Append new fields to `UiInputState` (after `activate`):
```cpp
    // M63 additions (appended to preserve aggregate-init order):
    bool  mouseDown = false;      // left button held this frame
    bool  mouseReleased = false;  // left button went up this frame
    bool  doubleClick = false;    // left double-click recognized this frame (host-timed)
    float wheel = 0.0f;           // scroll delta this frame (+ = up)
```
Add new types before `UiInputResult`:
```cpp
// Carried drag state: persists across the per-frame rebuild like focus.
struct UiDragState {
    bool          active = false;
    UiId          sourceId = 0;
    std::uint32_t sourceUserData = 0;
    Vec2          grabOffset{0.0f, 0.0f};   // cursor - sourceRect.min at pickup
};

// A completed drop (source slot dragged onto target slot); userData payloads.
struct UiDropEvent { std::uint32_t source = 0; std::uint32_t target = 0; };
```
Append to `UiInputResult` (after `focused`):
```cpp
    // M63 additions:
    std::optional<UiDropEvent>                 drop;          // a drop completed this frame
    std::optional<std::uint32_t>               quickTransfer; // double-click source userData
    std::vector<std::pair<UiId, float>>        scrollDeltas;  // {scrollBoxId, deltaPx}
    UiDragState                                drag;          // state to carry to next frame
```
Change `updateUi` signature (add params with defaults so existing callers compile):
```cpp
UiInputResult updateUi(const UiElement& root, const UiLayoutMap& rects,
                       const UiInputState& in, UiId prevFocused,
                       const UiDragState& prevDrag = {},
                       const UiClipMap& clips = {});
```

- [ ] **Step 4: Extend `engine/ui/UiInput.cpp`**

Add a collector for draggable/droppable/scrollbox elements and a clip-aware contains. Replace the file body's logic so `updateUi` does buttons (unchanged) PLUS the new interactions. Add near the top anon namespace:
```cpp
struct Hit { UiId id; std::uint32_t userData; };

bool containsClipped(const Rect& r, Vec2 p, const UiClipMap& clips, UiId id) {
    if (!(p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y)) return false;
    const auto it = clips.find(id);
    if (it != clips.end()) {
        const Rect& c = it->second;
        if (!(p.x >= c.min.x && p.x <= c.max.x && p.y >= c.min.y && p.y <= c.max.y)) return false;
    }
    return true;
}

void collectFlagged(const UiElement& e, std::vector<Hit>& drag, std::vector<Hit>& drop,
                    std::vector<UiId>& scrolls) {
    if (!e.visible) return;
    if (e.draggable)  drag.push_back({e.id, e.userData});
    if (e.dropTarget) drop.push_back({e.id, e.userData});
    if (e.kind == UiKind::ScrollBox) scrolls.push_back(e.id);
    for (const UiElement& c : e.children) collectFlagged(c, drag, drop, scrolls);
}
```
At the end of `updateUi`, before `return res;`, add (and set `res.drag = prevDrag;` near the top after constructing `res`):
```cpp
    // ---- M63: drag / drop / double-click / wheel ----
    res.drag = prevDrag;
    std::vector<Hit> draggables, dropTargets;
    std::vector<UiId> scrollBoxes;
    collectFlagged(root, draggables, dropTargets, scrollBoxes);

    // Topmost (last in draw order) hit under the cursor, respecting clips.
    const auto topHit = [&](const std::vector<Hit>& v) -> const Hit* {
        const Hit* found = nullptr;
        for (const Hit& h : v) {
            const auto it = rects.find(h.id);
            if (it != rects.end() && containsClipped(it->second, in.mouse, clips, h.id)) found = &h;
        }
        return found;
    };

    if (res.drag.active) {
        if (in.mouseReleased) {
            const Hit* tgt = topHit(dropTargets);
            if (tgt) res.drop = UiDropEvent{res.drag.sourceUserData, tgt->userData};
            res.drag = UiDragState{};                       // end drag
        }
    } else {
        const Hit* over = topHit(draggables);
        if (over && in.doubleClick) {
            res.quickTransfer = over->userData;             // double-click: quick-transfer
        } else if (over && in.mousePressed) {               // press: pick up
            const auto it = rects.find(over->id);
            res.drag.active = true;
            res.drag.sourceId = over->id;
            res.drag.sourceUserData = over->userData;
            res.drag.grabOffset = (it != rects.end())
                ? Vec2{in.mouse.x - it->second.min.x, in.mouse.y - it->second.min.y}
                : Vec2{0, 0};
        }
    }

    // Wheel -> scroll delta for the topmost scrollbox under the cursor.
    if (in.wheel != 0.0f) {
        UiId target = 0;
        for (UiId id : scrollBoxes) {
            const auto it = rects.find(id);
            if (it != rects.end()) {
                const Rect& r = it->second;
                if (in.mouse.x >= r.min.x && in.mouse.x <= r.max.x &&
                    in.mouse.y >= r.min.y && in.mouse.y <= r.max.y) target = id;
            }
        }
        if (target != 0) {
            const float kStep = 30.0f;                      // px per wheel notch
            res.scrollDeltas.push_back({target, -in.wheel * kStep});
        }
    }
```
Note: keep the early `if (buttons.empty()) return res;` from M62 from short-circuiting the new logic. **Remove that early return** — instead guard the button-specific block: wrap the button hover/focus/fire code in `if (!buttons.empty()) { ... }` so a buttonless screen (a pure inventory) still runs drag/drop. The drag/drop block above must run unconditionally.

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_ui` then `build-vk\tests\Debug\test_ui.exe`
Expected: PASS (drag/drop, double-click, wheel, ghost).

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiInput.h engine/ui/UiInput.cpp tests/test_ui.cpp
git commit -m "M63: UiInput drag/drop + double-click + wheel + clip-aware hit-testing + tests"
```

---

## Task 6: UiStack — carry scroll offsets + drag + cursor; composite ghost

**Files:**
- Modify: `engine/ui/UiStack.h`, `engine/ui/UiStack.cpp`
- Test: `tests/test_ui.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/test_ui.cpp`

```cpp
    // M63: UiStack accumulates wheel into a per-scrollbox offset across frames,
    // and exposes drag state that survives a clear()+rebuild re-seed.
    {
        auto buildScreen = []() {
            UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0});
            UiElement box = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, 0.0f);
            UiElement grid = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 300}, 1, 0.0f);
            for (int i = 0; i < 6; ++i)
                grid.children.push_back(uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                                Vec4{1, 1, 1, 1}));
            box.children.push_back(std::move(grid));
            root.children.push_back(std::move(box));
            return root;
        };
        UiStack stack;
        stack.push(buildScreen(), false);
        // Find the scrollbox id (child 0 of root).
        const UiId boxId = stack.top().children[0].id;

        UiInputState s; s.mouse = Vec2{50, 50}; s.wheel = -1.0f;  // scroll down a notch
        stack.update(s, Vec2{200, 200});
        CHECK(stack.scrollOffset(boxId) > 0.0f);                 // offset accumulated

        // Drag carries: seed a drag, confirm topDrag() reports it.
        UiDragState d; d.active = true; d.sourceId = boxId; d.sourceUserData = 7u;
        stack.setTopDrag(d);
        CHECK(stack.topDrag().active);
        CHECK(stack.topDrag().sourceUserData == 7u);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_ui`
Expected: FAIL — `scrollOffset`/`setTopDrag`/`topDrag` undeclared.

- [ ] **Step 3: Extend `engine/ui/UiStack.h`**

Add includes (already has UiInput.h). In the class public section, after the focus accessors:
```cpp
    // Carried drag state (persists across the per-frame rebuild like focus).
    UiDragState topDrag() const { return drag_; }
    void setTopDrag(const UiDragState& d) { drag_ = d; }

    // Per-scrollbox vertical scroll offset (px), accumulated from wheel input.
    float scrollOffset(UiId scrollBoxId) const {
        const auto it = scrollOffsets_.find(scrollBoxId);
        return it != scrollOffsets_.end() ? it->second : 0.0f;
    }
    void setScrollOffset(UiId scrollBoxId, float px) { scrollOffsets_[scrollBoxId] = px; }
```
Add private members (after `topHovered_`):
```cpp
    std::unordered_map<UiId, float> scrollOffsets_;
    UiDragState drag_;
    Vec2 lastMouse_{0.0f, 0.0f};
```
Add `#include <unordered_map>`.

Note: `clear()` must NOT wipe `scrollOffsets_`/`drag_` (they carry across the per-frame rebuild, exactly like the demo re-seeds focus). Leave them; the demo resets them on screen/mode changes.

- [ ] **Step 4: Extend `engine/ui/UiStack.cpp`**

Add `#include "ui/UiLayout.h"` (present). Update `update`:
```cpp
std::vector<std::uint32_t> UiStack::update(const UiInputState& in, Vec2 screenSize) {
    if (screens_.empty()) return {};
    Screen& s = screens_.back();
    UiLayoutMap m = layoutUi(s.root, screenSize);
    const UiClipMap clips = applyScroll(s.root, m, scrollOffsets_);
    const UiInputResult r = updateUi(s.root, m, in, s.focused, drag_, clips);
    s.focused = r.focused;
    topHovered_ = r.hovered;
    drag_ = r.drag;
    lastMouse_ = in.mouse;
    for (const auto& d : r.scrollDeltas) scrollOffsets_[d.first] += d.second;
    // Note: the drop / quickTransfer events are read by the caller via a
    // separate updateDetailed() if needed; see below.
    return r.fired;
}
```
**Problem:** the demo needs `r.drop` / `r.quickTransfer`, but `update` only returns `fired`. Add a richer entry point and keep `update` as a thin wrapper:
```cpp
UiInputResult UiStack::updateDetailed(const UiInputState& in, Vec2 screenSize) {
    if (screens_.empty()) return {};
    Screen& s = screens_.back();
    UiLayoutMap m = layoutUi(s.root, screenSize);
    const UiClipMap clips = applyScroll(s.root, m, scrollOffsets_);
    UiInputResult r = updateUi(s.root, m, in, s.focused, drag_, clips);
    s.focused = r.focused;
    topHovered_ = r.hovered;
    drag_ = r.drag;
    lastMouse_ = in.mouse;
    for (const auto& d : r.scrollDeltas) scrollOffsets_[d.first] += d.second;
    return r;
}

std::vector<std::uint32_t> UiStack::update(const UiInputState& in, Vec2 screenSize) {
    return updateDetailed(in, screenSize).fired;
}
```
Declare `UiInputResult updateDetailed(const UiInputState& in, Vec2 screenSize);` in the header (public). Update `render` to apply clips and draw the ghost on top:
```cpp
HudBatch UiStack::render(const FontAtlas& atlas, TextureHandle whiteTexture,
                         Vec2 screenSize) const {
    HudBatch out;
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        const Screen& s = screens_[i];
        const bool isTop = (i + 1 == screens_.size());
        UiLayoutMap m = layoutUi(s.root, screenSize);
        const UiClipMap clips = applyScroll(s.root, m, scrollOffsets_);
        const HudBatch b = renderUi(s.root, m, atlas, whiteTexture,
                                    isTop ? topHovered_ : 0,
                                    isTop ? s.focused : 0, clips);
        out.insert(out.end(), b.begin(), b.end());
    }
    // Drag ghost from the top screen, on top of everything.
    if (drag_.active && !screens_.empty()) {
        const Screen& s = screens_.back();
        UiLayoutMap m = layoutUi(s.root, screenSize);
        applyScroll(s.root, m, scrollOffsets_);
        const HudBatch g = renderUiDragGhost(s.root, m, atlas, whiteTexture, drag_, lastMouse_);
        out.insert(out.end(), g.begin(), g.end());
    }
    return out;
}
```
`render` is `const` but mutates nothing persistent (local `m`); `applyScroll` takes `UiLayoutMap&` (the local) and `const` offsets — fine.

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_ui` then `build-vk\tests\Debug\test_ui.exe`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiStack.h engine/ui/UiStack.cpp tests/test_ui.cpp
git commit -m "M63: UiStack scroll-offset + drag carry + updateDetailed + composite ghost + tests"
```

---

## Task 7: Input — mouse-released edge + scroll wheel

**Files:**
- Modify: `engine/core/Input.h`, `engine/core/Input.cpp`

This is GLFW plumbing — **not unit-tested** (no headless GLFW). Verified manually via the demo (Task 8). Keep the change minimal and self-contained.

- [ ] **Step 1: Add `mouseButtonReleased` (mirror of `keyReleased`)**

In `Input.h`, after `mouseButtonPressed`:
```cpp
    bool mouseButtonReleased(int button) const;  // went up this frame
    double scrollDelta() const { return scrollThisFrame_; }  // wheel since last update()
```
Add private members (after `previousMouse_`):
```cpp
    double scrollAccum_ = 0.0;       // accumulated by the GLFW scroll callback
    double scrollThisFrame_ = 0.0;   // snapshot taken in update()
```

- [ ] **Step 2: Add the released accessor + scroll callback in `Input.cpp`**

Add a self-contained instance registry + scroll callback (avoids clobbering the GLFW window user pointer, and chains any previously-installed scroll callback, e.g. ImGui's):
```cpp
#include <unordered_map>

namespace {
std::unordered_map<GLFWwindow*, iron::Input*>& inputRegistry() {
    static std::unordered_map<GLFWwindow*, iron::Input*> r;
    return r;
}
GLFWscrollfun g_prevScroll = nullptr;
void scrollCallback(GLFWwindow* w, double xoff, double yoff) {
    auto& reg = inputRegistry();
    const auto it = reg.find(w);
    if (it != reg.end() && it->second) it->second->addScroll(yoff);
    if (g_prevScroll) g_prevScroll(w, xoff, yoff);   // chain (ImGui etc.)
}
}  // namespace
```
In the constructor (inside `if (window_)`):
```cpp
        inputRegistry()[window_] = this;
        g_prevScroll = glfwSetScrollCallback(window_, scrollCallback);
```
In `update()`, after the mouse-button loop:
```cpp
    scrollThisFrame_ = scrollAccum_;
    scrollAccum_ = 0.0;
```
Add the methods:
```cpp
bool Input::mouseButtonReleased(int button) const {
    return button >= 0 && button < kMouseButtonCount
           && !currentMouse_[button] && previousMouse_[button];
}
void Input::addScroll(double yoff) { scrollAccum_ += yoff; }
```
Declare `void addScroll(double yoff);` in `Input.h` public section (needed by the file-local callback). Add `#include <unordered_map>` is in the .cpp; the registry approach keeps everything in the .cpp.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore`
Expected: builds clean (no new warnings beyond the pre-existing baseline).

- [ ] **Step 4: Commit**

```bash
git add engine/core/Input.h engine/core/Input.cpp
git commit -m "M63: Input mouseButtonReleased edge + scroll-wheel delta (chained callback)"
```

---

## Task 8: Demo — `games/13-loot`

**Files:**
- Create: `games/13-loot/main.cpp`, `games/13-loot/CMakeLists.txt`
- Create: `games/13-loot/assets/ui/panel9.png`, `games/13-loot/assets/ui/slot9.png`
- Create: `games/13-loot/assets/items/*.png` (6 icons) **or** generate solid tiles in code
- Create: `games/13-loot/assets/fonts/Roboto-Medium.ttf` (copy from `games/12-ui-arena/assets/fonts/`)
- Modify: root `CMakeLists.txt` (`add_subdirectory(games/13-loot)`)

Model the scaffold (Application/Renderer/Camera/FontAtlas setup, render loop, drawHud-before-endFrame) on `games/12-ui-arena/main.cpp`. Key differences below.

- [ ] **Step 1: Create assets**

- Copy the font: `games/12-ui-arena/assets/fonts/Roboto-Medium.ttf` → `games/13-loot/assets/fonts/Roboto-Medium.ttf`.
- For `panel9.png` / `slot9.png`: small (e.g. 32×32) rounded-border tiles with an ~8px border (so `nineSliceUv = 8/32 = 0.25`). If authoring PNGs is impractical, **fall back to generating a tile in code** (a white rounded-rect-ish RGBA buffer uploaded via `createTexture`) — see Step 3's `makeTileTexture` note. Item icons: 6 small PNGs in `assets/items/`, OR generate solid-color tiles with the item's first letter drawn — keep it simple; the engine is icon-agnostic.

**Decision to keep the task self-contained:** generate the slot/panel tile and item icons procedurally in code (no binary assets to author), and only ship the font. This avoids checking in PNGs and keeps the demo deterministic.

- [ ] **Step 2: `games/13-loot/CMakeLists.txt`**

```cmake
add_executable(loot main.cpp)
target_link_libraries(loot PRIVATE ironcore)

add_custom_command(TARGET loot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:loot>/assets
  COMMENT "Copying loot assets")
```
And in root `CMakeLists.txt`, after `add_subdirectory(games/12-ui-arena)`:
```cmake
add_subdirectory(games/13-loot)
```

- [ ] **Step 3: `games/13-loot/main.cpp`**

Concrete structure (fill the scaffold from `12-ui-arena`):

```cpp
// games/13-loot/main.cpp — M63 rich inventory UI demo: open a chest, drag/double-
// click items into your backpack. Press E to open/close; world freezes while open.
#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "game/Inventory.h"
#include "math/Transform.h"
#include "render/RendererFactory.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiStack.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace iron;

constexpr std::uint32_t CHEST = 0, BACKPACK = 1;
constexpr std::uint32_t slotUserData(std::uint32_t container, int idx) {
    return (container << 16) | static_cast<std::uint32_t>(idx);
}
constexpr std::uint32_t udContainer(std::uint32_t ud) { return ud >> 16; }
constexpr int           udIndex(std::uint32_t ud) { return static_cast<int>(ud & 0xFFFF); }

// Build one slot (9-slice tile + icon + count label) bound to inv[idx].
UiElement buildSlot(std::uint32_t container, int idx, const Inventory& inv,
                    const ItemDefTable& defs, TextureHandle tile) {
    UiElement slot = uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{48, 48}, tile,
                            Vec4{10, 10, 10, 10}, 0.25f, slotUserData(container, idx),
                            Vec4{0.20f, 0.21f, 0.25f, 1.0f});
    const ItemStack& s = inv.at(idx);
    if (!s.empty()) {
        const ItemDef& def = defs.get(s.item);
        slot.children.push_back(uiImage(Anchor::Center, Vec2{0, 0}, Vec2{32, 32},
                                        def.icon, Vec4{1, 1, 1, 1}));
        if (s.count > 1) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", s.count);
            slot.children.push_back(uiLabel(Anchor::BottomRight, Vec2{-14, -16}, buf, 14.0f,
                                            Vec4{1, 1, 1, 1}));
        }
    }
    return slot;
}

UiElement buildLootScreen(const Inventory& chest, const Inventory& pack,
                          const ItemDefTable& defs, TextureHandle panel, TextureHandle tile) {
    UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0.45f});
    // Chest panel (left) with a scrollbox + grid.
    UiElement chestPanel = uiImage9(Anchor::Center, Vec2{-180, 0}, Vec2{260, 320}, panel,
                                    Vec4{12, 12, 12, 12}, 0.25f, Vec4{1, 1, 1, 1});
    chestPanel.children.push_back(uiLabel(Anchor::TopCenter, Vec2{-30, 12}, "CHEST", 18.0f,
                                          Vec4{0.85f, 0.88f, 0.95f, 1}));
    UiElement chestScroll = uiScrollBox(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 6.0f);
    UiElement chestGrid = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{228, 800}, 4, 6.0f);
    for (int i = 0; i < chest.size(); ++i)
        chestGrid.children.push_back(buildSlot(CHEST, i, chest, defs, tile));
    chestScroll.children.push_back(std::move(chestGrid));
    chestPanel.children.push_back(std::move(chestScroll));
    root.children.push_back(std::move(chestPanel));
    // Backpack panel (right), plain grid (no scroll).
    UiElement packPanel = uiImage9(Anchor::Center, Vec2{180, 0}, Vec2{260, 320}, panel,
                                   Vec4{12, 12, 12, 12}, 0.25f, Vec4{1, 1, 1, 1});
    packPanel.children.push_back(uiLabel(Anchor::TopCenter, Vec2{-44, 12}, "BACKPACK", 18.0f,
                                         Vec4{0.85f, 0.88f, 0.95f, 1}));
    UiElement packGrid = uiGrid(Anchor::TopLeft, Vec2{16, 44}, Vec2{228, 256}, 4, 6.0f);
    for (int i = 0; i < pack.size(); ++i)
        packGrid.children.push_back(buildSlot(BACKPACK, i, pack, defs, tile));
    packPanel.children.push_back(std::move(packGrid));
    root.children.push_back(std::move(packPanel));
    return root;
}
}  // namespace
```

Then `main()` (model on 12-ui-arena):
1. Create Application/Renderer/Camera, bake the font (same block as 12-ui-arena, repointed to `13-loot`).
2. **Generate textures in code:** `makeTileTexture()` → a 32×32 RGBA rounded-ish white tile uploaded with `createTexture(...,/*srgb=*/false)` for `panel`/`tile`; 6 item icons via `makeIconTexture(color)` → small solid-color RGBA tiles (also srgb=false). Build `ItemDefTable` with ids 1..6, names, maxStack (e.g. potion 10, coin 99, others 1), and the generated `icon` handles.
3. Seed `Inventory chest(24)` (more than the visible ~16 → scrolls) with a spread of items; `Inventory pack(16)` mostly empty.
4. State: `bool open = false;`. Carry focus/scroll/drag is internal to `UiStack` now — but reset on open/close: when toggling open, call `stack.clear(); stack.setTopDrag({});` and reset scroll offsets you track (simplest: keep the same `UiStack` and just `clear()`; offsets persist which is fine, or call `setScrollOffset(id,0)` — acceptable either way).
5. Per-frame update:
   - `if (input.keyPressed(GLFW_KEY_E)) open = !open;` and `if (open && input.keyPressed(GLFW_KEY_ESCAPE)) open = false;`
   - Spin the chest mesh only while `!open` (freeze world when looting).
   - Rebuild the stack each frame from the models: `stack.clear(); if (open) stack.push(buildLootScreen(chest, pack, defs, panel, tile), /*modal=*/true);`
   - Map input → `UiInputState`: `mouse`, `mousePressed = input.mouseButtonPressed(LEFT)`, `mouseDown = input.mouseButtonDown(LEFT)`, `mouseReleased = input.mouseButtonReleased(LEFT)`, `wheel = (float)input.scrollDelta()`, and **doubleClick**: track `lastClickTime`/`accumTime` with `time.deltaSeconds`; set `doubleClick = mousePressed && (now - lastClick) < 0.30`; update `lastClick` on each `mousePressed`.
   - `if (open) { const UiInputResult r = stack.updateDetailed(ui, screen); applyEvents(r, chest, pack, defs); }`
   - `applyEvents`: for `r.drop`, decode source/target userData; pick inventories by container; `Inventory::moveTo(srcInv, srcSlot, dstInv, dstSlot, defs)`. For `r.quickTransfer`, decode source; transfer to the *other* container via `Inventory::quickTransfer(srcInv, srcSlot, dstInv, defs)`.
6. Render (model on 12-ui-arena): `beginFrame`; submit chest cube when `!open` (or always — your call); `const HudBatch hud = stack.render(atlas, renderer.whiteTexture(), screen); renderer.drawHud(hud, w, h);` **before** `endFrame()`.

Provide concrete `makeTileTexture`/`makeIconTexture`/`applyEvents` implementations inline (small RGBA fills; `applyEvents` is ~15 lines of decode + the two static calls above).

- [ ] **Step 4: Build + manual smoke**

Run: `cmake --build build-vk --target loot`
Expected: builds. Run `build-vk\games\13-loot\Debug\loot.exe`: press **E** → looting screen; drag an item chest→backpack; double-click an item → it jumps to the other grid; mouse-wheel over the chest grid scrolls it; **E**/**Esc** closes; cube spins only when closed.

- [ ] **Step 5: Commit**

```bash
git add games/13-loot CMakeLists.txt
git commit -m "M63: games/13-loot demo — chest looting (drag + double-click + scroll, 9-slice)"
```

---

## Final verification (before the visual gate / PR)

- [ ] **Clean full build** (catches stale-binary / interface-break issues per the project's CI-readiness rule):

Run: `cmake --build build-vk` (all targets)
Expected: builds clean; only the pre-existing `fopen` C4996 warning (matching other games) is acceptable.

- [ ] **Run the full test suite** — every `test_*.exe` in `build-vk\tests\Debug\`, especially `test_inventory` and `test_ui`. All print `OK - all checks passed`.

- [ ] **Visual gate:** hand off `build-vk\games\13-loot\Debug\loot.exe` to the user to confirm the four behaviors (open, drag-transfer, double-click-transfer, scroll) and the 9-slice look.

---

## Self-review notes (author)

- **Spec coverage:** Inventory model (T1) ✓; Grid (T3) ✓; ScrollBox + clip (T3/T4) ✓; 9-slice (T2 fields, T4 render) ✓; drag/drop + double-click (T5) ✓; carried scroll/drag/ghost (T6) ✓; wheel input (T7) ✓; demo (T8) ✓.
- **Deliberate spec deviation:** drag starts on *press over a draggable* (press-to-lift), not a 4px move threshold — simpler, fully testable, and the double-click path covers accidental drags (a press+release on the same slot yields a `drop{source==target}` no-op, which `applyEvents` ignores because `moveTo(&s==&d)` returns false / decodes to same slot).
- **9-slice convention:** `nineSliceMargin` = destination border px, `nineSliceUv` = uniform source border fraction (0..1). Authoring tiles use border = uv*texSize. Tunable at the visual gate.
- **Back-compat:** all `UiInputState`/`UiInputResult` additions are appended; `updateUi`/`renderUi` new params are defaulted; `layoutUi` is unchanged (scroll/clip is the separate `applyScroll` pass). Existing `12-ui-arena` and M62 tests keep compiling.
- **Type consistency:** `applyScroll(root, UiLayoutMap&, offsets) -> UiClipMap`; `updateUi(..., prevDrag, clips)`; `renderUi(..., clips)`; `renderUiDragGhost(root, rects, atlas, white, drag, cursor)`; `UiStack::updateDetailed` returns the full `UiInputResult`. Names match across tasks.
