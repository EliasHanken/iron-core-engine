# M62 — Runtime Game UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a retained, data-driven runtime UI system (widget tree + layout + input + crisp TTF text) and ship a demo game with a Main Menu → HUD → Pause flow.

**Architecture:** A headless widget tree (`UiElement`) is the single source of truth, like `GraphEditorModel` for node graphs. Pure passes — layout (anchor/box/stack) → input (hit-test/focus-nav) → render (→ `HudBatch`) — keep everything unit-testable; rendering reuses the existing `HudBatch` → `VkHud` path. Text comes from a `stb_truetype`-baked `FontAtlas`. A `UiStack` manages modal screens. The legacy `Hud`/`BitmapFont` is untouched so the six games using it keep working.

**Tech Stack:** C++17, CMake, Vulkan (canonical `build-vk`), `stb_truetype` (vendored alongside `stb_image`), nlohmann/json, the `iron_test` harness.

---

## File Structure

**New engine modules (`engine/ui/`):**
- `UiElement.h` — widget struct + enums + builder free functions + `uiAssignIds` + `uiEqual` (header-only).
- `UiLayout.h/.cpp` — `Rect`, `UiLayoutMap`, `layoutUi` (anchor + stretch + stack).
- `UiInput.h/.cpp` — `UiInputState`, `UiInputResult`, `updateUi` (hit-test, click, focus-nav).
- `FontAtlas.h/.cpp` — bake a TTF → proportional atlas + glyph metrics.
- `UiRender.h/.cpp` — laid-out tree → `HudBatch`.
- `UiStack.h/.cpp` — modal screen stack (input routing + render order).
- `UiSerialize.h/.cpp` — JSON round-trip.

**New third-party (vendored single header):**
- `third_party/stb/stb_truetype.h` + `third_party/stb/stb_truetype.cpp` (implementation TU).

**New demo game:**
- `games/12-ui-arena/main.cpp` + `CMakeLists.txt` + `assets/fonts/Roboto-Medium.ttf`.

**New test:** `tests/test_ui.cpp`.

**Modified:** `engine/CMakeLists.txt` (new `ui/*.cpp`), `third_party/CMakeLists.txt` (stb_truetype), `tests/CMakeLists.txt` (test_ui), root `CMakeLists.txt` (game subdir).

---

## Task 1: UiElement data model + builders

**Files:**
- Create: `engine/ui/UiElement.h`
- Create: `tests/test_ui.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `engine/ui/UiElement.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace iron {

using UiId = std::uint32_t;   // 1-based; 0 = unset/invalid

enum class UiKind { Panel, Label, Image, Button, Bar };

// 9 presets place the element's matching point against the parent rect;
// Stretch fills the parent (offset acts as a uniform inset).
enum class Anchor {
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight,
    Stretch
};

enum class StackDir { None, Vertical, Horizontal };

struct UiElement {
    UiId          id      = 0;
    UiKind        kind    = UiKind::Panel;
    Anchor        anchor  = Anchor::TopLeft;
    Vec2          offset{0.0f, 0.0f};
    Vec2          size{0.0f, 0.0f};            // ignored when anchor == Stretch
    Vec4          color{1.0f, 1.0f, 1.0f, 1.0f}; // panel bg / text / image tint / bar fill
    bool          visible = true;

    StackDir      stack   = StackDir::None;    // Panel only: flow children
    float         spacing = 0.0f;

    std::string   text;                        // Label, Button
    float         fontPx  = 18.0f;             // Label, Button
    TextureHandle texture = kInvalidHandle;    // Image
    float         value   = 0.0f;              // Bar fill 0..1
    Vec4          trackColor{0.0f, 0.0f, 0.0f, 1.0f}; // Bar background
    std::uint32_t actionId = 0;                // Button -> game action code

    std::vector<UiElement> children;
};

// ---- builders (value-returning so trees read top-down) ----
inline UiElement uiPanel(Anchor a, Vec2 offset, Vec2 size, Vec4 color) {
    UiElement e; e.kind = UiKind::Panel; e.anchor = a; e.offset = offset;
    e.size = size; e.color = color; return e;
}
inline UiElement uiStackPanel(Anchor a, Vec2 offset, Vec2 size, StackDir dir, float spacing) {
    UiElement e = uiPanel(a, offset, size, Vec4{0, 0, 0, 0});
    e.stack = dir; e.spacing = spacing; return e;
}
inline UiElement uiLabel(Anchor a, Vec2 offset, std::string text, float fontPx, Vec4 color) {
    UiElement e; e.kind = UiKind::Label; e.anchor = a; e.offset = offset;
    e.text = std::move(text); e.fontPx = fontPx; e.color = color; return e;
}
inline UiElement uiImage(Anchor a, Vec2 offset, Vec2 size, TextureHandle tex, Vec4 tint) {
    UiElement e; e.kind = UiKind::Image; e.anchor = a; e.offset = offset;
    e.size = size; e.texture = tex; e.color = tint; return e;
}
inline UiElement uiButton(Anchor a, Vec2 offset, Vec2 size, std::string text,
                          float fontPx, std::uint32_t actionId, Vec4 color) {
    UiElement e; e.kind = UiKind::Button; e.anchor = a; e.offset = offset;
    e.size = size; e.text = std::move(text); e.fontPx = fontPx;
    e.actionId = actionId; e.color = color; return e;
}
inline UiElement uiBar(Anchor a, Vec2 offset, Vec2 size, float value, Vec4 fill, Vec4 track) {
    UiElement e; e.kind = UiKind::Bar; e.anchor = a; e.offset = offset;
    e.size = size; e.value = value; e.color = fill; e.trackColor = track; return e;
}

// Assign unique sequential ids (1-based), depth-first. Returns next free id.
// Call once after building a screen.
inline UiId uiAssignIds(UiElement& root, UiId next = 1) {
    root.id = next++;
    for (UiElement& c : root.children) next = uiAssignIds(c, next);
    return next;
}

// Deep structural equality with float tolerance (for serialization tests).
// Ignores `id` (re-assigned on load) and `texture` (runtime handle).
inline bool uiEqual(const UiElement& a, const UiElement& b) {
    auto nf = [](float x, float y) { return std::fabs(x - y) <= 1e-4f; };
    auto n2 = [&](Vec2 x, Vec2 y) { return nf(x.x, y.x) && nf(x.y, y.y); };
    auto n4 = [&](Vec4 x, Vec4 y) {
        return nf(x.x, y.x) && nf(x.y, y.y) && nf(x.z, y.z) && nf(x.w, y.w);
    };
    if (a.kind != b.kind || a.anchor != b.anchor || a.stack != b.stack) return false;
    if (!n2(a.offset, b.offset) || !n2(a.size, b.size)) return false;
    if (!n4(a.color, b.color) || !n4(a.trackColor, b.trackColor)) return false;
    if (a.visible != b.visible || a.text != b.text) return false;
    if (!nf(a.fontPx, b.fontPx) || !nf(a.value, b.value) || !nf(a.spacing, b.spacing)) return false;
    if (a.actionId != b.actionId) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i)
        if (!uiEqual(a.children[i], b.children[i])) return false;
    return true;
}

}  // namespace iron
```

- [ ] **Step 2: Write the failing test in `tests/test_ui.cpp`**

```cpp
#include "ui/UiElement.h"
#include "test_framework.h"

using namespace iron;

int main() {
    // Builders set kind + fields; a stack panel groups children.
    {
        UiElement menu = uiStackPanel(Anchor::Center, Vec2{0, 0}, Vec2{200, 160},
                                      StackDir::Vertical, 8.0f);
        menu.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Play", 20.0f, 1, Vec4{0.2f, 0.2f, 0.25f, 1}));
        menu.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Quit", 20.0f, 2, Vec4{0.2f, 0.2f, 0.25f, 1}));

        CHECK(menu.kind == UiKind::Panel);
        CHECK(menu.stack == StackDir::Vertical);
        CHECK(menu.children.size() == 2u);
        CHECK(menu.children[0].kind == UiKind::Button);
        CHECK(menu.children[0].actionId == 1u);
        CHECK(menu.children[1].text == "Quit");

        // Ids are unique + sequential, depth-first.
        const UiId next = uiAssignIds(menu);
        CHECK(menu.id == 1u);
        CHECK(menu.children[0].id == 2u);
        CHECK(menu.children[1].id == 3u);
        CHECK(next == 4u);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

Add this line right after the `iron_add_test(test_hud test_hud.cpp)` line:

```cmake
iron_add_test(test_ui test_ui.cpp)
```

- [ ] **Step 4: Configure + build the test**

Run: `cmake --build build-vk --config Debug --target test_ui`
Expected: compiles and links (it only needs the header + ironcore).

> If CMake doesn't pick up the new test target, re-run configure first:
> `cmake -S . -B build-vk` then rebuild.

- [ ] **Step 5: Run the test**

Run: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiElement.h tests/test_ui.cpp tests/CMakeLists.txt
git commit -m "M62: UiElement widget model + builders + test_ui scaffold"
```

---

## Task 2: UiLayout (anchor + stretch + stack)

**Files:**
- Create: `engine/ui/UiLayout.h`, `engine/ui/UiLayout.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Write `engine/ui/UiLayout.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "ui/UiElement.h"

#include <unordered_map>

namespace iron {

// A pixel rectangle, top-left origin (x right, y down) — same space as HudVertex.
struct Rect { Vec2 min; Vec2 max; };
inline Vec2 rectSize(const Rect& r) { return Vec2{r.max.x - r.min.x, r.max.y - r.min.y}; }
inline Vec2 rectCenter(const Rect& r) {
    return Vec2{(r.min.x + r.max.x) * 0.5f, (r.min.y + r.max.y) * 0.5f};
}

using UiLayoutMap = std::unordered_map<UiId, Rect>;

// Resolve screen rects for `root` and every descendant. Anchors resolve against
// the parent rect; Stretch fills the parent inset by `offset`; Stack panels flow
// children along the stack axis (cross axis still honors each child's anchor).
// The tree must have ids assigned (uiAssignIds) — elements with id 0 are skipped.
UiLayoutMap layoutUi(const UiElement& root, Vec2 screenSize);

}  // namespace iron
```

- [ ] **Step 2: Write `engine/ui/UiLayout.cpp`**

```cpp
#include "ui/UiLayout.h"

namespace iron {

namespace {

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };

HAlign hOf(Anchor a) {
    switch (a) {
        case Anchor::TopLeft: case Anchor::CenterLeft: case Anchor::BottomLeft:
            return HAlign::Left;
        case Anchor::TopRight: case Anchor::CenterRight: case Anchor::BottomRight:
            return HAlign::Right;
        default: return HAlign::Center;
    }
}
VAlign vOf(Anchor a) {
    switch (a) {
        case Anchor::TopLeft: case Anchor::TopCenter: case Anchor::TopRight:
            return VAlign::Top;
        case Anchor::BottomLeft: case Anchor::BottomCenter: case Anchor::BottomRight:
            return VAlign::Bottom;
        default: return VAlign::Center;
    }
}

// Place a `size`-sized box against `parent` per `anchor`, then shift by `offset`.
Rect resolveAnchored(const Rect& parent, Anchor anchor, Vec2 size, Vec2 offset) {
    if (anchor == Anchor::Stretch) {
        return Rect{Vec2{parent.min.x + offset.x, parent.min.y + offset.y},
                    Vec2{parent.max.x - offset.x, parent.max.y - offset.y}};
    }
    const Vec2 ps = rectSize(parent);
    float minX = parent.min.x;
    switch (hOf(anchor)) {
        case HAlign::Left:   minX = parent.min.x; break;
        case HAlign::Center: minX = parent.min.x + ps.x * 0.5f - size.x * 0.5f; break;
        case HAlign::Right:  minX = parent.max.x - size.x; break;
    }
    float minY = parent.min.y;
    switch (vOf(anchor)) {
        case VAlign::Top:    minY = parent.min.y; break;
        case VAlign::Center: minY = parent.min.y + ps.y * 0.5f - size.y * 0.5f; break;
        case VAlign::Bottom: minY = parent.max.y - size.y; break;
    }
    minX += offset.x;
    minY += offset.y;
    return Rect{Vec2{minX, minY}, Vec2{minX + size.x, minY + size.y}};
}

void layoutRec(const UiElement& e, const Rect& eRect, UiLayoutMap& out) {
    if (e.id != 0) out[e.id] = eRect;

    if (e.stack == StackDir::None) {
        for (const UiElement& c : e.children)
            layoutRec(c, resolveAnchored(eRect, c.anchor, c.size, c.offset), out);
        return;
    }

    // Stack: position along the main axis with a running cursor; resolve the
    // cross axis from each child's anchor.
    const bool vertical = (e.stack == StackDir::Vertical);
    float cursor = vertical ? eRect.min.y : eRect.min.x;
    for (const UiElement& c : e.children) {
        float minX, minY;
        if (vertical) {
            const Rect cross = resolveAnchored(eRect, c.anchor, c.size, Vec2{c.offset.x, 0.0f});
            minX = cross.min.x;
            minY = cursor + c.offset.y;
            cursor += c.size.y + e.spacing;
        } else {
            const Rect cross = resolveAnchored(eRect, c.anchor, c.size, Vec2{0.0f, c.offset.y});
            minY = cross.min.y;
            minX = cursor + c.offset.x;
            cursor += c.size.x + e.spacing;
        }
        const Rect cr{Vec2{minX, minY}, Vec2{minX + c.size.x, minY + c.size.y}};
        layoutRec(c, cr, out);
    }
}

}  // namespace

UiLayoutMap layoutUi(const UiElement& root, Vec2 screenSize) {
    UiLayoutMap out;
    const Rect screen{Vec2{0.0f, 0.0f}, screenSize};
    const Rect rootRect = resolveAnchored(screen, root.anchor, root.size, root.offset);
    layoutRec(root, rootRect, out);
    return out;
}

}  // namespace iron
```

- [ ] **Step 3: Add the sources to `engine/CMakeLists.txt`**

Add after the `ui/Hud.cpp` line (line 24):

```cmake
  ui/UiLayout.cpp
```

- [ ] **Step 4: Append layout tests to `tests/test_ui.cpp`**

Add this block immediately before `return iron_test_result();`:

```cpp
    // Layout: anchors resolve against the parent; Stretch insets by offset.
    {
        UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0},
                                 Vec4{0, 0, 0, 0});
        // top-left 100x40 at (10,10)
        root.children.push_back(uiPanel(Anchor::TopLeft, Vec2{10, 10}, Vec2{100, 40},
                                        Vec4{1, 1, 1, 1}));
        // bottom-right 50x50 inset (20,20) from the corner
        root.children.push_back(uiPanel(Anchor::BottomRight, Vec2{-20, -20}, Vec2{50, 50},
                                        Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        const Rect rr = m.at(root.id);
        CHECK_NEAR(rr.min.x, 0.0f); CHECK_NEAR(rr.max.x, 800.0f);

        const Rect tl = m.at(root.children[0].id);
        CHECK_NEAR(tl.min.x, 10.0f); CHECK_NEAR(tl.min.y, 10.0f);
        CHECK_NEAR(tl.max.x, 110.0f); CHECK_NEAR(tl.max.y, 50.0f);

        const Rect br = m.at(root.children[1].id);
        CHECK_NEAR(br.max.x, 800.0f - 20.0f);  // right edge minus offset
        CHECK_NEAR(br.max.y, 600.0f - 20.0f);
        CHECK_NEAR(br.min.x, 800.0f - 20.0f - 50.0f);
    }

    // Layout: a vertical stack places children top-down with spacing.
    {
        UiElement stack = uiStackPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 300},
                                       StackDir::Vertical, 10.0f);
        stack.children.push_back(uiPanel(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         Vec4{1, 1, 1, 1}));
        stack.children.push_back(uiPanel(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         Vec4{1, 1, 1, 1}));
        uiAssignIds(stack);
        const UiLayoutMap m = layoutUi(stack, Vec2{800, 600});

        const Rect a = m.at(stack.children[0].id);
        const Rect b = m.at(stack.children[1].id);
        CHECK_NEAR(a.min.y, 0.0f);
        CHECK_NEAR(b.min.y, 50.0f);           // 40 height + 10 spacing
        CHECK_NEAR(a.min.x, 10.0f);           // centered: (200-180)/2
        CHECK_NEAR(b.min.x, 10.0f);
    }
```

- [ ] **Step 5: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiLayout.h engine/ui/UiLayout.cpp engine/CMakeLists.txt tests/test_ui.cpp
git commit -m "M62: UiLayout — anchor/stretch/stack layout pass + tests"
```

---

## Task 3: UiInput (hit-test, click, focus navigation)

**Files:**
- Create: `engine/ui/UiInput.h`, `engine/ui/UiInput.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Write `engine/ui/UiInput.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "ui/UiElement.h"
#include "ui/UiLayout.h"

#include <cstdint>
#include <vector>

namespace iron {

// One frame of UI input. Edge flags (`*Pressed`) are true only on the frame the
// key/button transitions down — the game fills these from iron::Input.
struct UiInputState {
    Vec2 mouse{0.0f, 0.0f};
    bool mousePressed = false;   // left mouse went down this frame
    bool navPrev = false;        // up/left went down this frame
    bool navNext = false;        // down/right went down this frame
    bool activate = false;       // enter/space/gamepad-A went down this frame
};

struct UiInputResult {
    std::vector<std::uint32_t> fired;  // actionIds activated this frame
    UiId hovered = 0;                  // button under the cursor (0 = none)
    UiId focused = 0;                  // keyboard/gamepad focus (0 = none)
};

// Process one screen's tree. `prevFocused` carries focus across frames. Mouse:
// the topmost visible Button under the cursor is `hovered`; a click on it fires
// its actionId and takes focus. Keyboard/gamepad: navNext/navPrev move focus
// among visible Buttons in tree order (wrapping); activate fires the focused
// button. A button fires at most once per frame.
UiInputResult updateUi(const UiElement& root, const UiLayoutMap& rects,
                       const UiInputState& in, UiId prevFocused);

}  // namespace iron
```

- [ ] **Step 2: Write `engine/ui/UiInput.cpp`**

```cpp
#include "ui/UiInput.h"

#include <algorithm>

namespace iron {

namespace {

struct Focusable { UiId id; std::uint32_t actionId; };

bool contains(const Rect& r, Vec2 p) {
    return p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y;
}

// Collect visible Buttons in depth-first (draw) order. A hidden element prunes
// its whole subtree.
void collect(const UiElement& e, std::vector<Focusable>& out) {
    if (!e.visible) return;
    if (e.kind == UiKind::Button) out.push_back({e.id, e.actionId});
    for (const UiElement& c : e.children) collect(c, out);
}

}  // namespace

UiInputResult updateUi(const UiElement& root, const UiLayoutMap& rects,
                       const UiInputState& in, UiId prevFocused) {
    UiInputResult res;
    std::vector<Focusable> buttons;
    collect(root, buttons);
    if (buttons.empty()) return res;

    // Hover: topmost (last in draw order) button whose rect contains the cursor.
    for (const Focusable& b : buttons) {
        const auto it = rects.find(b.id);
        if (it != rects.end() && contains(it->second, in.mouse)) res.hovered = b.id;
    }

    // Focus starts from last frame; default to the first button if unset.
    res.focused = prevFocused;
    const auto indexOf = [&](UiId id) -> int {
        for (std::size_t i = 0; i < buttons.size(); ++i)
            if (buttons[i].id == id) return static_cast<int>(i);
        return -1;
    };
    int fi = indexOf(res.focused);

    if (in.navNext) {
        fi = (fi < 0) ? 0 : (fi + 1) % static_cast<int>(buttons.size());
        res.focused = buttons[fi].id;
    } else if (in.navPrev) {
        fi = (fi < 0) ? static_cast<int>(buttons.size()) - 1
                      : (fi - 1 + static_cast<int>(buttons.size())) % static_cast<int>(buttons.size());
        res.focused = buttons[fi].id;
    }

    // Mouse click on the hovered button: fire + focus.
    if (in.mousePressed && res.hovered != 0) {
        const int hi = indexOf(res.hovered);
        if (hi >= 0) {
            res.fired.push_back(buttons[static_cast<std::size_t>(hi)].actionId);
            res.focused = res.hovered;
            fi = hi;
        }
    }

    // Activate the focused button (keyboard/gamepad). Avoid double-firing if the
    // same button was just mouse-clicked.
    if (in.activate && fi >= 0) {
        const std::uint32_t a = buttons[static_cast<std::size_t>(fi)].actionId;
        if (res.fired.empty() || res.fired.back() != a) res.fired.push_back(a);
    }

    return res;
}

}  // namespace iron
```

- [ ] **Step 3: Add to `engine/CMakeLists.txt`**

Add after the `ui/UiLayout.cpp` line:

```cmake
  ui/UiInput.cpp
```

- [ ] **Step 4: Append input tests to `tests/test_ui.cpp`** (before `return iron_test_result();`)

```cpp
    // Input: a click inside a button fires its actionId; outside does not.
    {
        UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                         "Play", 18.0f, 7, Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        UiInputState in;
        in.mouse = Vec2{50, 25}; in.mousePressed = true;
        UiInputResult r = updateUi(root, m, in, 0);
        CHECK(r.hovered == root.children[0].id);
        CHECK(r.fired.size() == 1u);
        CHECK(r.fired[0] == 7u);

        in.mouse = Vec2{500, 500};   // outside the button
        r = updateUi(root, m, in, 0);
        CHECK(r.hovered == 0u);
        CHECK(r.fired.empty());
    }

    // Input: nav cycles focus among buttons (wrapping); activate fires focused.
    {
        UiElement root = uiStackPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200},
                                      StackDir::Vertical, 0.0f);
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 40},
                                         "A", 18.0f, 11, Vec4{1, 1, 1, 1}));
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 40},
                                         "B", 18.0f, 22, Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});
        const UiId bA = root.children[0].id;
        const UiId bB = root.children[1].id;

        UiInputState nav; nav.navNext = true;
        UiInputResult r = updateUi(root, m, nav, 0);
        CHECK(r.focused == bA);                 // first navNext focuses first button
        r = updateUi(root, m, nav, r.focused);
        CHECK(r.focused == bB);                 // advances
        r = updateUi(root, m, nav, r.focused);
        CHECK(r.focused == bA);                 // wraps

        UiInputState act; act.activate = true;
        r = updateUi(root, m, act, bB);
        CHECK(r.focused == bB);
        CHECK(r.fired.size() == 1u);
        CHECK(r.fired[0] == 22u);
    }
```

- [ ] **Step 5: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiInput.h engine/ui/UiInput.cpp engine/CMakeLists.txt tests/test_ui.cpp
git commit -m "M62: UiInput — hit-test, click-to-fire, focus navigation + tests"
```

---

## Task 4: FontAtlas (stb_truetype bake)

**Files:**
- Create: `third_party/stb/stb_truetype.h` (copy of vendored single header), `third_party/stb/stb_truetype.cpp`
- Create: `engine/ui/FontAtlas.h`, `engine/ui/FontAtlas.cpp`
- Modify: `third_party/CMakeLists.txt`, `engine/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Vendor `stb_truetype.h`**

Copy the single header that vcpkg already provides into the vendored stb dir:

```bash
cp build-vk/vcpkg_installed/x64-windows/include/stb_truetype.h third_party/stb/stb_truetype.h
```

Expected: `third_party/stb/stb_truetype.h` now exists (a ~5000-line public-domain header).

- [ ] **Step 2: Write `third_party/stb/stb_truetype.cpp`** (the single implementation TU)

```cpp
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
```

- [ ] **Step 3: Add the implementation TU to the stb library in `third_party/CMakeLists.txt`**

Change the first line from:

```cmake
add_library(stb_image STATIC stb/stb_image.cpp)
```

to:

```cmake
add_library(stb_image STATIC stb/stb_image.cpp stb/stb_truetype.cpp)
```

(The existing `/W0` and PUBLIC include dir already cover the new file, and `ironcore` already links `stb_image`.)

- [ ] **Step 4: Write `engine/ui/FontAtlas.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

#include <string>
#include <vector>

namespace iron {

// One laid-out glyph: pixel rect (in text-local space, baseline-relative) and
// atlas UVs. A whitespace glyph has min == max (nothing to draw) but still
// advances the pen.
struct GlyphQuad { Vec2 min, max, uvMin, uvMax; };

// A proportional font atlas baked from a TTF at one pixel height via
// stb_truetype. Holds CPU-side RGBA8 pixels (white, alpha = coverage) plus
// per-glyph metrics for printable ASCII (32..126). The caller uploads `pixels()`
// through Renderer::createTexture (srgb=false) and assigns the handle to
// `texture`.
class FontAtlas {
public:
    bool bake(const unsigned char* ttf, int ttfLen, float pixelHeight);

    // Append the quad for `c` and advance the pen (in baked-pixel space, with the
    // baseline at penY). Unknown glyphs leave the pen unchanged.
    GlyphQuad quadFor(char c, float& penX, float& penY) const;
    float textWidth(const std::string& text) const;   // baked-size advance width
    float pixelHeight() const { return pixelHeight_; }

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<unsigned char>& pixels() const { return pixels_; }  // RGBA8

    TextureHandle texture = kInvalidHandle;   // set by the caller after upload

private:
    static constexpr int kFirst = 32;
    static constexpr int kCount = 95;         // 32..126 inclusive

    struct Glyph {
        int   x0 = 0, y0 = 0, x1 = 0, y1 = 0; // atlas pixel box
        float xoff = 0, yoff = 0, xadvance = 0;
    };

    std::vector<Glyph> glyphs_;               // size kCount
    int   width_ = 0, height_ = 0;
    float pixelHeight_ = 0.0f;
    std::vector<unsigned char> pixels_;
};

}  // namespace iron
```

- [ ] **Step 5: Write `engine/ui/FontAtlas.cpp`**

```cpp
#include "ui/FontAtlas.h"

#include <stb_truetype.h>   // declarations; implementation is in third_party/stb

namespace iron {

bool FontAtlas::bake(const unsigned char* ttf, int ttfLen, float pixelHeight) {
    if (!ttf || ttfLen <= 0 || pixelHeight <= 0.0f) return false;

    width_ = 512;
    height_ = 512;
    pixelHeight_ = pixelHeight;

    std::vector<unsigned char> coverage(static_cast<std::size_t>(width_) * height_, 0);
    std::vector<stbtt_bakedchar> cd(kCount);
    const int rows = stbtt_BakeFontBitmap(ttf, 0, pixelHeight, coverage.data(),
                                          width_, height_, kFirst, kCount, cd.data());
    if (rows == 0) return false;   // not a single row fit -> bad font / size

    // Expand 8-bit coverage to RGBA8 white (tinted later by vertex color).
    pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        pixels_[i * 4 + 0] = 255;
        pixels_[i * 4 + 1] = 255;
        pixels_[i * 4 + 2] = 255;
        pixels_[i * 4 + 3] = coverage[i];
    }

    glyphs_.resize(kCount);
    for (int i = 0; i < kCount; ++i) {
        const stbtt_bakedchar& b = cd[i];
        Glyph g;
        g.x0 = b.x0; g.y0 = b.y0; g.x1 = b.x1; g.y1 = b.y1;
        g.xoff = b.xoff; g.yoff = b.yoff; g.xadvance = b.xadvance;
        glyphs_[i] = g;
    }
    return true;
}

GlyphQuad FontAtlas::quadFor(char c, float& penX, float& penY) const {
    GlyphQuad q{};
    const int idx = static_cast<unsigned char>(c) - kFirst;
    if (idx < 0 || idx >= kCount || glyphs_.empty()) return q;

    const Glyph& g = glyphs_[static_cast<std::size_t>(idx)];
    const float ipw = 1.0f / static_cast<float>(width_);
    const float iph = 1.0f / static_cast<float>(height_);

    const float rx = penX + g.xoff;
    const float ry = penY + g.yoff;
    q.min   = Vec2{rx, ry};
    q.max   = Vec2{rx + static_cast<float>(g.x1 - g.x0), ry + static_cast<float>(g.y1 - g.y0)};
    q.uvMin = Vec2{static_cast<float>(g.x0) * ipw, static_cast<float>(g.y0) * iph};
    q.uvMax = Vec2{static_cast<float>(g.x1) * ipw, static_cast<float>(g.y1) * iph};

    penX += g.xadvance;
    return q;
}

float FontAtlas::textWidth(const std::string& text) const {
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) quadFor(c, penX, penY);
    return penX;
}

}  // namespace iron
```

- [ ] **Step 6: Add `ui/FontAtlas.cpp` to `engine/CMakeLists.txt`** (after `ui/UiInput.cpp`)

```cmake
  ui/FontAtlas.cpp
```

- [ ] **Step 7: Give `test_ui` the repo-root define so it can load the TTF**

In `tests/CMakeLists.txt`, replace the `iron_add_test(test_ui test_ui.cpp)` line with:

```cmake
iron_add_test(test_ui test_ui.cpp)
target_compile_definitions(test_ui PRIVATE IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
```

- [ ] **Step 8: Append FontAtlas tests to `tests/test_ui.cpp`**

Add this include near the top with the others:

```cpp
#include "ui/FontAtlas.h"
#include <cstdio>
#include <string>
#include <vector>
```

Add this block before `return iron_test_result();`:

```cpp
    // FontAtlas: baking Roboto yields a non-empty atlas + sane metrics.
    {
        const std::string path =
            std::string(IRON_REPO_ROOT) + "/games/11-sandbox/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            CHECK(rd == bytes.size());

            FontAtlas atlas;
            CHECK(atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f));
            CHECK(atlas.width() > 0);
            CHECK(atlas.height() > 0);
            CHECK(atlas.pixels().size() ==
                  static_cast<std::size_t>(atlas.width()) * atlas.height() * 4);
            CHECK(atlas.textWidth("AV") > 0.0f);
            CHECK(atlas.textWidth("AVA") > atlas.textWidth("AV"));  // proportional advance

            float penX = 0.0f, penY = 0.0f;
            atlas.quadFor('A', penX, penY);
            CHECK(penX > 0.0f);   // pen advanced
        }
    }
```

- [ ] **Step 9: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 10: Commit**

```bash
git add third_party/stb/stb_truetype.h third_party/stb/stb_truetype.cpp third_party/CMakeLists.txt \
        engine/ui/FontAtlas.h engine/ui/FontAtlas.cpp engine/CMakeLists.txt \
        tests/test_ui.cpp tests/CMakeLists.txt
git commit -m "M62: FontAtlas — stb_truetype-baked proportional atlas + tests"
```

---

## Task 5: UiRender (tree → HudBatch)

**Files:**
- Create: `engine/ui/UiRender.h`, `engine/ui/UiRender.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Write `engine/ui/UiRender.h`**

```cpp
#pragma once

#include "render/HudBatch.h"
#include "render/Handles.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiLayout.h"

namespace iron {

// Build screen-space HUD geometry for a laid-out tree. Panels/Bars/Buttons emit
// colored quads (via `whiteTexture`); Images use their own texture; Labels and
// Buttons draw text via `atlas`. The `hovered`/`focused` ids get a brighter
// overlay on Buttons. Hidden elements (and their subtrees) are skipped.
HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused);

}  // namespace iron
```

- [ ] **Step 2: Write `engine/ui/UiRender.cpp`**

```cpp
#include "ui/UiRender.h"

namespace iron {

namespace {

void appendQuad(std::vector<HudVertex>& out, Vec2 min, Vec2 max,
                Vec2 uvMin, Vec2 uvMax, Vec4 color) {
    const HudVertex tl{Vec2{min.x, min.y}, Vec2{uvMin.x, uvMin.y}, color};
    const HudVertex tr{Vec2{max.x, min.y}, Vec2{uvMax.x, uvMin.y}, color};
    const HudVertex br{Vec2{max.x, max.y}, Vec2{uvMax.x, uvMax.y}, color};
    const HudVertex bl{Vec2{min.x, max.y}, Vec2{uvMin.x, uvMax.y}, color};
    out.push_back(tl); out.push_back(bl); out.push_back(br);
    out.push_back(tl); out.push_back(br); out.push_back(tr);
}

std::vector<HudVertex>& groupFor(HudBatch& batch, TextureHandle tex) {
    for (HudDrawGroup& g : batch)
        if (g.texture == tex) return g.vertices;
    batch.push_back(HudDrawGroup{tex, {}});
    return batch.back().vertices;
}

// Draw text with its top-left at `topLeft`, scaled so the em-height is `fontPx`.
void appendText(HudBatch& batch, const FontAtlas& atlas, const std::string& text,
                Vec2 topLeft, float fontPx, Vec4 color) {
    if (atlas.texture == kInvalidHandle || atlas.pixelHeight() <= 0.0f) return;
    const float scale = fontPx / atlas.pixelHeight();
    // Baseline ~0.8 em below the top so glyphs sit inside the rect.
    const Vec2 origin{topLeft.x, topLeft.y + fontPx * 0.8f};
    std::vector<HudVertex>& verts = groupFor(batch, atlas.texture);
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) {
        const GlyphQuad q = atlas.quadFor(c, penX, penY);
        if (q.min.x == q.max.x || q.min.y == q.max.y) continue;  // whitespace
        appendQuad(verts, origin + q.min * scale, origin + q.max * scale,
                   q.uvMin, q.uvMax, color);
    }
}

void renderRec(const UiElement& e, const UiLayoutMap& rects, const FontAtlas& atlas,
               TextureHandle white, UiId hovered, UiId focused, HudBatch& batch) {
    if (!e.visible) return;
    const auto it = rects.find(e.id);
    if (it == rects.end()) { for (const auto& c : e.children) renderRec(c, rects, atlas, white, hovered, focused, batch); return; }
    const Rect r = it->second;

    switch (e.kind) {
        case UiKind::Panel:
            if (e.color.w > 0.0f)
                appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        case UiKind::Image:
            appendQuad(groupFor(batch, e.texture), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        case UiKind::Bar: {
            appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.trackColor);
            const float frac = e.value < 0.0f ? 0.0f : (e.value > 1.0f ? 1.0f : e.value);
            const Vec2 fillMax{r.min.x + rectSize(r).x * frac, r.max.y};
            if (frac > 0.0f)
                appendQuad(groupFor(batch, white), r.min, fillMax, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        }
        case UiKind::Button: {
            Vec4 bg = e.color;
            if (e.id == hovered || e.id == focused) {   // brighten when active
                bg.x = bg.x + (1.0f - bg.x) * 0.25f;
                bg.y = bg.y + (1.0f - bg.y) * 0.25f;
                bg.z = bg.z + (1.0f - bg.z) * 0.25f;
            }
            appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, bg);
            // Centered caption.
            const float w = atlas.textWidth(e.text) * (e.fontPx / atlas.pixelHeight());
            const Vec2 c = rectCenter(r);
            appendText(batch, atlas, e.text, Vec2{c.x - w * 0.5f, c.y - e.fontPx * 0.5f},
                       e.fontPx, Vec4{1, 1, 1, 1});
            break;
        }
        case UiKind::Label:
            appendText(batch, atlas, e.text, r.min, e.fontPx, e.color);
            break;
    }

    for (const UiElement& c : e.children)
        renderRec(c, rects, atlas, white, hovered, focused, batch);
}

}  // namespace

HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused) {
    HudBatch batch;
    renderRec(root, rects, atlas, whiteTexture, hovered, focused, batch);
    return batch;
}

}  // namespace iron
```

- [ ] **Step 3: Add `ui/UiRender.cpp` to `engine/CMakeLists.txt`** (after `ui/FontAtlas.cpp`)

```cmake
  ui/UiRender.cpp
```

- [ ] **Step 4: Append render tests to `tests/test_ui.cpp`**

Add this include near the top:

```cpp
#include "ui/UiRender.h"
```

Add before `return iron_test_result();`:

```cpp
    // Render: a panel emits one white-texture quad (6 verts) at its laid-out rect.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 40},
                                 Vec4{0.5f, 0.5f, 0.5f, 1.0f});
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        FontAtlas empty;                 // not baked: text routines no-op
        const TextureHandle white = static_cast<TextureHandle>(1);
        const HudBatch b = renderUi(root, m, empty, white, 0, 0);

        CHECK(b.size() == 1u);
        CHECK(b[0].texture == white);
        CHECK(b[0].vertices.size() == 6u);
        CHECK_NEAR(b[0].vertices[0].position.x, 0.0f);
        CHECK_NEAR(b[0].vertices[0].position.y, 0.0f);
    }

    // Render: a Bar emits the track quad plus a partial fill quad (2 quads).
    {
        UiElement bar = uiBar(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 20}, 0.5f,
                              Vec4{1, 0, 0, 1}, Vec4{0.2f, 0.2f, 0.2f, 1});
        uiAssignIds(bar);
        const UiLayoutMap m = layoutUi(bar, Vec2{800, 600});
        FontAtlas empty;
        const TextureHandle white = static_cast<TextureHandle>(1);
        const HudBatch b = renderUi(bar, m, empty, white, 0, 0);

        CHECK(b.size() == 1u);                  // both quads use whiteTexture
        CHECK(b[0].vertices.size() == 12u);     // track + fill = 2 quads
    }
```

- [ ] **Step 5: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiRender.h engine/ui/UiRender.cpp engine/CMakeLists.txt tests/test_ui.cpp
git commit -m "M62: UiRender — laid-out tree to HudBatch quads + text + tests"
```

---

## Task 6: UiStack (modal screens)

**Files:**
- Create: `engine/ui/UiStack.h`, `engine/ui/UiStack.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Write `engine/ui/UiStack.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/HudBatch.h"
#include "render/Handles.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"

#include <cstdint>
#include <vector>

namespace iron {

// A stack of UI screens. Screens render bottom-to-top. Input routes to the TOP
// screen only — so a modal pause menu pushed over the HUD automatically blocks
// the HUD (and the game checks topIsModal() to freeze the world).
class UiStack {
public:
    std::size_t push(UiElement root, bool modal);   // (re)assigns ids; returns index
    void pop();
    void clear();
    bool empty() const { return screens_.empty(); }
    std::size_t size() const { return screens_.size(); }
    bool topIsModal() const { return !screens_.empty() && screens_.back().modal; }

    UiElement&       top()       { return screens_.back().root; }
    const UiElement& top() const { return screens_.back().root; }

    // Route input to the top screen; returns its fired actionIds.
    std::vector<std::uint32_t> update(const UiInputState& in, Vec2 screenSize);
    // Render every screen bottom-to-top into one batch.
    HudBatch render(const FontAtlas& atlas, TextureHandle whiteTexture, Vec2 screenSize) const;

private:
    struct Screen { UiElement root; bool modal = false; UiId focused = 0; };
    std::vector<Screen> screens_;
    UiId topHovered_ = 0;
};

}  // namespace iron
```

- [ ] **Step 2: Write `engine/ui/UiStack.cpp`**

```cpp
#include "ui/UiStack.h"

#include "ui/UiLayout.h"
#include "ui/UiRender.h"

#include <utility>

namespace iron {

std::size_t UiStack::push(UiElement root, bool modal) {
    Screen s;
    s.root = std::move(root);
    s.modal = modal;
    uiAssignIds(s.root);
    screens_.push_back(std::move(s));
    return screens_.size() - 1;
}

void UiStack::pop() { if (!screens_.empty()) screens_.pop_back(); }
void UiStack::clear() { screens_.clear(); topHovered_ = 0; }

std::vector<std::uint32_t> UiStack::update(const UiInputState& in, Vec2 screenSize) {
    if (screens_.empty()) return {};
    Screen& s = screens_.back();
    const UiLayoutMap m = layoutUi(s.root, screenSize);
    const UiInputResult r = updateUi(s.root, m, in, s.focused);
    s.focused = r.focused;
    topHovered_ = r.hovered;
    return r.fired;
}

HudBatch UiStack::render(const FontAtlas& atlas, TextureHandle whiteTexture,
                         Vec2 screenSize) const {
    HudBatch out;
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        const Screen& s = screens_[i];
        const bool isTop = (i + 1 == screens_.size());
        const UiLayoutMap m = layoutUi(s.root, screenSize);
        const HudBatch b = renderUi(s.root, m, atlas, whiteTexture,
                                    isTop ? topHovered_ : 0,
                                    isTop ? s.focused : 0);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

}  // namespace iron
```

- [ ] **Step 3: Add `ui/UiStack.cpp` to `engine/CMakeLists.txt`** (after `ui/UiRender.cpp`)

```cmake
  ui/UiStack.cpp
```

- [ ] **Step 4: Append stack tests to `tests/test_ui.cpp`**

Add the include near the top:

```cpp
#include "ui/UiStack.h"
```

Add before `return iron_test_result();`:

```cpp
    // UiStack: a modal top screen blocks input to the screen beneath it.
    {
        UiStack stack;
        UiElement hud = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0});
        hud.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                        "HudBtn", 18.0f, 99, Vec4{1, 1, 1, 1}));
        stack.push(hud, /*modal=*/false);

        UiElement pause = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0.5f});
        pause.children.push_back(uiButton(Anchor::Center, Vec2{0, 0}, Vec2{120, 40},
                                          "Resume", 18.0f, 3, Vec4{1, 1, 1, 1}));
        stack.push(pause, /*modal=*/true);
        CHECK(stack.topIsModal());

        // Click where the HUD button sits (top-left): it must NOT fire — only the
        // top (pause) screen receives input, and nothing of pause is there.
        UiInputState in; in.mouse = Vec2{50, 25}; in.mousePressed = true;
        std::vector<std::uint32_t> fired = stack.update(in, Vec2{800, 600});
        CHECK(fired.empty());

        // Pop the pause screen; now the HUD button is clickable again.
        stack.pop();
        CHECK(!stack.topIsModal());
        fired = stack.update(in, Vec2{800, 600});
        CHECK(fired.size() == 1u);
        CHECK(fired[0] == 99u);
    }
```

- [ ] **Step 5: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiStack.h engine/ui/UiStack.cpp engine/CMakeLists.txt tests/test_ui.cpp
git commit -m "M62: UiStack — modal screen stack, input routing + render order + tests"
```

---

## Task 7: UiSerialize (JSON round-trip)

**Files:**
- Create: `engine/ui/UiSerialize.h`, `engine/ui/UiSerialize.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: `tests/test_ui.cpp`

- [ ] **Step 1: Write `engine/ui/UiSerialize.h`**

```cpp
#pragma once

#include "ui/UiElement.h"

#include <nlohmann/json.hpp>

namespace iron {

// Serialize a widget tree to/from JSON. Enums are stored as ints; `texture` (a
// runtime handle) and `id` (re-assigned on load via uiAssignIds) are not
// persisted. fromJson is tolerant: missing keys fall back to defaults.
nlohmann::json uiToJson(const UiElement& e);
UiElement uiFromJson(const nlohmann::json& j);

}  // namespace iron
```

- [ ] **Step 2: Write `engine/ui/UiSerialize.cpp`**

```cpp
#include "ui/UiSerialize.h"

namespace iron {

namespace {

nlohmann::json v2(Vec2 v) { return nlohmann::json{{"x", v.x}, {"y", v.y}}; }
nlohmann::json v4(Vec4 v) { return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}}; }

Vec2 readV2(const nlohmann::json& j, Vec2 def) {
    if (!j.is_object()) return def;
    return Vec2{j.value("x", def.x), j.value("y", def.y)};
}
Vec4 readV4(const nlohmann::json& j, Vec4 def) {
    if (!j.is_object()) return def;
    return Vec4{j.value("x", def.x), j.value("y", def.y), j.value("z", def.z), j.value("w", def.w)};
}

}  // namespace

nlohmann::json uiToJson(const UiElement& e) {
    nlohmann::json j;
    j["kind"]       = static_cast<int>(e.kind);
    j["anchor"]     = static_cast<int>(e.anchor);
    j["offset"]     = v2(e.offset);
    j["size"]       = v2(e.size);
    j["color"]      = v4(e.color);
    j["visible"]    = e.visible;
    j["stack"]      = static_cast<int>(e.stack);
    j["spacing"]    = e.spacing;
    j["text"]       = e.text;
    j["fontPx"]     = e.fontPx;
    j["value"]      = e.value;
    j["trackColor"] = v4(e.trackColor);
    j["actionId"]   = e.actionId;

    nlohmann::json kids = nlohmann::json::array();
    for (const UiElement& c : e.children) kids.push_back(uiToJson(c));
    j["children"] = std::move(kids);
    return j;
}

UiElement uiFromJson(const nlohmann::json& j) {
    UiElement e;
    e.kind       = static_cast<UiKind>(j.value("kind", 0));
    e.anchor     = static_cast<Anchor>(j.value("anchor", 0));
    e.offset     = readV2(j.contains("offset") ? j["offset"] : nlohmann::json{}, Vec2{0, 0});
    e.size       = readV2(j.contains("size") ? j["size"] : nlohmann::json{}, Vec2{0, 0});
    e.color      = readV4(j.contains("color") ? j["color"] : nlohmann::json{}, Vec4{1, 1, 1, 1});
    e.visible    = j.value("visible", true);
    e.stack      = static_cast<StackDir>(j.value("stack", 0));
    e.spacing    = j.value("spacing", 0.0f);
    e.text       = j.value("text", std::string());
    e.fontPx     = j.value("fontPx", 18.0f);
    e.value      = j.value("value", 0.0f);
    e.trackColor = readV4(j.contains("trackColor") ? j["trackColor"] : nlohmann::json{}, Vec4{0, 0, 0, 1});
    e.actionId   = j.value("actionId", 0u);

    if (j.contains("children") && j["children"].is_array())
        for (const auto& jc : j["children"]) e.children.push_back(uiFromJson(jc));
    return e;
}

}  // namespace iron
```

- [ ] **Step 3: Add `ui/UiSerialize.cpp` to `engine/CMakeLists.txt`** (after `ui/UiStack.cpp`)

```cmake
  ui/UiSerialize.cpp
```

- [ ] **Step 4: Append serialization test to `tests/test_ui.cpp`**

Add the include near the top:

```cpp
#include "ui/UiSerialize.h"
```

Add before `return iron_test_result();`:

```cpp
    // Serialize: a built tree round-trips through JSON (uiEqual ignores id/texture).
    {
        UiElement root = uiStackPanel(Anchor::Center, Vec2{0, 0}, Vec2{200, 160},
                                      StackDir::Vertical, 8.0f);
        root.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Play", 20.0f, 1, Vec4{0.2f, 0.2f, 0.25f, 1}));
        root.children.push_back(uiBar(Anchor::BottomLeft, Vec2{10, -10}, Vec2{120, 14},
                                      0.7f, Vec4{0.8f, 0.2f, 0.2f, 1}, Vec4{0.1f, 0.1f, 0.1f, 1}));
        uiAssignIds(root);

        const UiElement back = uiFromJson(uiToJson(root));
        CHECK(uiEqual(root, back));
    }
```

- [ ] **Step 5: Build + run**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target test_ui`
Then: `build-vk/tests/Debug/test_ui.exe`
Expected: `OK - all checks passed`

- [ ] **Step 6: Commit**

```bash
git add engine/ui/UiSerialize.h engine/ui/UiSerialize.cpp engine/CMakeLists.txt tests/test_ui.cpp
git commit -m "M62: UiSerialize — JSON round-trip for widget trees + test"
```

---

## Task 8: Demo game `games/12-ui-arena`

**Files:**
- Create: `games/12-ui-arena/main.cpp`, `games/12-ui-arena/CMakeLists.txt`
- Create: `games/12-ui-arena/assets/fonts/Roboto-Medium.ttf` (copy)
- Modify: root `CMakeLists.txt`

- [ ] **Step 1: Copy the font asset**

```bash
mkdir -p games/12-ui-arena/assets/fonts
cp games/11-sandbox/assets/fonts/Roboto-Medium.ttf games/12-ui-arena/assets/fonts/Roboto-Medium.ttf
```

- [ ] **Step 2: Write `games/12-ui-arena/main.cpp`**

```cpp
// games/12-ui-arena/main.cpp — M62 runtime-UI demo: Main Menu -> HUD -> Pause.
// Controls: mouse + Up/Down + Enter navigate menus; Esc toggles pause in-game;
// hold Space to "take damage" (drains the health bar).

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
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

enum class Mode { Menu, Playing, Paused };

// Button action codes.
constexpr std::uint32_t ACT_PLAY    = 1;
constexpr std::uint32_t ACT_QUIT    = 2;
constexpr std::uint32_t ACT_RESUME  = 3;
constexpr std::uint32_t ACT_TO_MENU = 4;

const iron::Vec4 kPanelBg{0.10f, 0.10f, 0.13f, 0.96f};
const iron::Vec4 kBtnBg{0.16f, 0.17f, 0.22f, 1.0f};
const iron::Vec4 kAccent{0.24f, 0.49f, 0.67f, 1.0f};
const iron::Vec4 kWhite{1, 1, 1, 1};

iron::UiElement buildMenu() {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0.06f, 0.07f, 0.09f, 1.0f});
    root.children.push_back(iron::uiLabel(iron::Anchor::Center, {-90, -150},
                                          "IRON ARENA", 48.0f, kWhite));
    iron::UiElement col = iron::uiStackPanel(iron::Anchor::Center, {0, -20}, {220, 150},
                                             iron::StackDir::Vertical, 12.0f);
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {220, 44},
                                          "Play", 22.0f, ACT_PLAY, kBtnBg));
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {220, 44},
                                          "Quit", 22.0f, ACT_QUIT, kBtnBg));
    root.children.push_back(std::move(col));
    return root;
}

iron::UiElement buildHud(float health, int ammo, int score, float timeSec) {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0, 0, 0, 0});
    // Top-left: health bar + ammo.
    root.children.push_back(iron::uiLabel(iron::Anchor::TopLeft, {16, 12}, "HEALTH", 16.0f,
                                          iron::Vec4{0.7f, 0.7f, 0.75f, 1}));
    root.children.push_back(iron::uiBar(iron::Anchor::TopLeft, {16, 34}, {220, 16}, health,
                                        iron::Vec4{0.78f, 0.26f, 0.24f, 1},
                                        iron::Vec4{0.18f, 0.10f, 0.11f, 1}));
    char ammoBuf[32];
    std::snprintf(ammoBuf, sizeof(ammoBuf), "AMMO  %d / 90", ammo);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopLeft, {16, 58}, ammoBuf, 18.0f, kWhite));
    // Top-right: score + time.
    char scoreBuf[32];
    std::snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d", score);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopRight, {-160, 12}, scoreBuf, 20.0f, kWhite));
    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "TIME  %02d:%02d",
                  static_cast<int>(timeSec) / 60, static_cast<int>(timeSec) % 60);
    root.children.push_back(iron::uiLabel(iron::Anchor::TopRight, {-160, 40}, timeBuf, 18.0f,
                                          iron::Vec4{0.7f, 0.7f, 0.75f, 1}));
    // Center crosshair.
    root.children.push_back(iron::uiPanel(iron::Anchor::Center, {-2, -2}, {4, 4}, kWhite));
    return root;
}

iron::UiElement buildPause() {
    iron::UiElement root = iron::uiPanel(iron::Anchor::Stretch, {0, 0}, {0, 0},
                                         iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});  // dim
    iron::UiElement panel = iron::uiPanel(iron::Anchor::Center, {0, 0}, {240, 200}, kPanelBg);
    panel.children.push_back(iron::uiLabel(iron::Anchor::TopCenter, {-40, 18}, "Paused", 26.0f, kWhite));
    iron::UiElement col = iron::uiStackPanel(iron::Anchor::TopCenter, {0, 64}, {210, 130},
                                             iron::StackDir::Vertical, 10.0f);
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {210, 40},
                                          "Resume", 20.0f, ACT_RESUME, kBtnBg));
    col.children.push_back(iron::uiButton(iron::Anchor::TopCenter, {0, 0}, {210, 40},
                                          "Quit to Menu", 20.0f, ACT_TO_MENU, kBtnBg));
    panel.children.push_back(std::move(col));
    root.children.push_back(std::move(panel));
    return root;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core - UI Arena (M62)";
    config.width = 1280;
    config.height = 720;
    iron::Application app(config);
    if (!app.valid()) { iron::Log::error("UI Arena: init failed"); return 1; }

    auto rendererPtr = iron::createRenderer(app.window());
    iron::Renderer& renderer = *rendererPtr;

    // --- Font atlas: bake Roboto and upload as a texture ---
    iron::FontAtlas atlas;
    {
        const std::string path = iron::executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            if (rd == bytes.size() &&
                atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f)) {
                atlas.texture = renderer.createTexture(atlas.width(), atlas.height(),
                                                       atlas.pixels().data(), /*srgb=*/false);
            }
        }
        if (atlas.texture == iron::kInvalidHandle)
            iron::Log::error("UI Arena: font atlas failed to load (text will be blank)");
    }

    // --- A trivial 3D scene behind the UI ---
    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader = renderer.createStandardLitShader();
    iron::Camera camera;
    camera.setTarget(iron::Vec3{0, 0, 0});
    camera.setDistance(4.0f);
    camera.setAspect(static_cast<float>(app.window().width()) /
                     static_cast<float>(app.window().height()));

    // --- UI / game state ---
    Mode mode = Mode::Menu;
    iron::UiStack stack;
    stack.push(buildMenu(), /*modal=*/false);

    float spin = 0.0f;
    float health = 1.0f;
    int   ammo = 24;
    int   score = 1250;
    float playTime = 0.0f;

    auto rebuildStack = [&]() {
        stack.clear();
        if (mode == Mode::Menu) {
            stack.push(buildMenu(), false);
        } else {
            stack.push(buildHud(health, ammo, score, playTime), false);
            if (mode == Mode::Paused) stack.push(buildPause(), /*modal=*/true);
        }
    };

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();

        // Esc: in-game toggles pause; in menu it quits.
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            if (mode == Mode::Playing) mode = Mode::Paused;
            else if (mode == Mode::Paused) mode = Mode::Playing;
            else glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        // Advance the world only while actively playing.
        if (mode == Mode::Playing) {
            spin += time.deltaSeconds;
            playTime += time.deltaSeconds;
            if (input.keyDown(GLFW_KEY_SPACE)) health -= time.deltaSeconds * 0.3f;
            else health += time.deltaSeconds * 0.1f;
            if (health < 0.0f) health = 0.0f;
            if (health > 1.0f) health = 1.0f;
        }

        // Rebuild screens each frame so HUD values + the active stack stay current.
        rebuildStack();

        // Translate engine input into a UiInputState.
        iron::UiInputState ui;
        ui.mouse = iron::Vec2{static_cast<float>(input.mouseX()),
                              static_cast<float>(input.mouseY())};
        ui.mousePressed = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        ui.navPrev = input.keyPressed(GLFW_KEY_UP) || input.keyPressed(GLFW_KEY_W);
        ui.navNext = input.keyPressed(GLFW_KEY_DOWN) || input.keyPressed(GLFW_KEY_S);
        ui.activate = input.keyPressed(GLFW_KEY_ENTER) || input.keyPressed(GLFW_KEY_SPACE);
        // (Space doubles as the "damage" key while playing; only let it activate
        //  buttons when a menu is actually up.)
        if (mode == Mode::Playing) ui.activate = input.keyPressed(GLFW_KEY_ENTER);

        const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                static_cast<float>(app.window().height())};
        const std::vector<std::uint32_t> fired = stack.update(ui, screen);
        for (std::uint32_t a : fired) {
            switch (a) {
                case ACT_PLAY:
                    mode = Mode::Playing; health = 1.0f; playTime = 0.0f; break;
                case ACT_QUIT:
                    glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE); break;
                case ACT_RESUME:
                    mode = Mode::Playing; break;
                case ACT_TO_MENU:
                    mode = Mode::Menu; break;
                default: break;
            }
        }
    });

    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.05f, 0.06f, 0.08f},
                            iron::DirectionalLight{},
                            std::span<const iron::PointLight>{},
                            iron::Fog{},
                            camera.viewMatrix(),
                            camera.projectionMatrix());
        if (mode != Mode::Menu) {
            iron::DrawCall call;
            call.mesh = cube;
            call.shader = shader;
            call.model = iron::rotationY(spin) * iron::rotationX(spin * 0.5f);
            renderer.submit(call);
        }

        const iron::Vec2 screen{static_cast<float>(app.window().width()),
                                static_cast<float>(app.window().height())};
        const iron::HudBatch hud = stack.render(atlas, renderer.whiteTexture(), screen);
        renderer.drawHud(hud, static_cast<int>(screen.x), static_cast<int>(screen.y));

        renderer.endFrame();
    });

    app.run();
    return 0;
}
```

- [ ] **Step 3: Write `games/12-ui-arena/CMakeLists.txt`**

```cmake
add_executable(ui-arena main.cpp)
target_link_libraries(ui-arena PRIVATE ironcore)

add_custom_command(TARGET ui-arena POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:ui-arena>/assets
  COMMENT "Copying ui-arena assets")
```

- [ ] **Step 4: Register the game in root `CMakeLists.txt`**

Add after the `add_subdirectory(games/11-sandbox)` line:

```cmake
add_subdirectory(games/12-ui-arena)
```

- [ ] **Step 5: Configure + build the game**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug --target ui-arena`
Expected: builds `build-vk/games/12-ui-arena/Debug/ui-arena.exe` and copies assets next to it.

- [ ] **Step 6: Commit**

```bash
git add games/12-ui-arena/main.cpp games/12-ui-arena/CMakeLists.txt \
        games/12-ui-arena/assets/fonts/Roboto-Medium.ttf CMakeLists.txt
git commit -m "M62: ui-arena demo game — Main Menu -> HUD -> Pause flow"
```

---

## Task 9: Full build + test sweep + visual-gate handoff

**Files:** none (verification only)

- [ ] **Step 1: Clean-configure + build everything**

Run: `cmake -S . -B build-vk && cmake --build build-vk --config Debug`
Expected: all targets compile and link (engine, editor, all games incl. ui-arena, all tests). No new warnings-as-errors.

> Per the "verify clean build before CI" memory: build ALL targets, not just
> the incremental ones, so a stale binary can't hide an interface break.

- [ ] **Step 2: Run the full test suite**

Run: `cd build-vk && ctest -C Debug && cd ..`
Expected: `100% tests passed` (the prior 74 + `test_ui` = 75).

- [ ] **Step 3: Launch the demo for the visual gate**

Run: `build-vk/games/12-ui-arena/Debug/ui-arena.exe`
Verify by hand:
- Main Menu shows crisp "IRON ARENA" title + Play/Quit buttons; mouse hover brightens; clicking **Play** enters the game; Up/Down + Enter also navigate.
- In-game HUD shows a health bar (top-left, drains while holding **Space**, regenerates otherwise), ammo, score, time, and a center crosshair, over the spinning cube.
- **Esc** opens a dimmed Pause overlay with Resume / Quit-to-Menu; the world stops spinning while paused; Resume returns to play; Quit-to-Menu returns to the menu.

- [ ] **Step 4: Commit any visual-gate tuning**

If text baseline/positions need nudging after seeing them (the `0.8f`/`0.5f`
heuristics in `UiRender.cpp::appendText` and button centering), adjust and:

```bash
git add -A
git commit -m "M62: visual-gate tuning for text baseline/placement"
```

---

## Self-Review

**Spec coverage:**
- Widget tree (Panel/Label/Image/Button/Bar) → Task 1. ✓
- Anchor + box + stack layout → Task 2. ✓
- Input: hit-test, hover/click, focus nav → Task 3. ✓
- Modal capture → Task 6 (UiStack routes input to top only). ✓
- TTF FontAtlas via stb_truetype → Task 4. ✓
- Render via HudBatch (reuse VkHud) → Task 5 + demo `drawHud`. ✓
- UiStack screens → Task 6. ✓
- JSON serialization → Task 7. ✓
- Demo `games/12-ui-arena` (Menu/HUD/Pause flow) → Task 8. ✓
- Headless `test_ui` (layout, input, focus-nav, modal capture, serialization) → Tasks 1–7. ✓
- Legacy `Hud`/`BitmapFont` untouched, 6 games unaffected → no task modifies them. ✓
- Out-of-scope items (editor builder, UI nodes, slider/checkbox, migrations) → not present. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code; every command has expected output. ✓

**Type consistency:** `UiElement`/`UiId`/`Anchor`/`StackDir`/`UiKind` consistent across all tasks. `Rect`/`UiLayoutMap`/`layoutUi` from Task 2 used unchanged in Tasks 3/5/6. `UiInputState`/`UiInputResult`/`updateUi` from Task 3 used in Task 6. `FontAtlas` API (`bake`/`quadFor`/`textWidth`/`pixelHeight`/`width`/`height`/`pixels`/`texture`) consistent across Tasks 4/5/8. `renderUi` signature identical in Tasks 5/6. Builder names (`uiPanel`/`uiStackPanel`/`uiLabel`/`uiImage`/`uiButton`/`uiBar`/`uiAssignIds`/`uiEqual`) consistent across all consumers. ✓

**Note on a deliberate test simplification:** Task 6's modal test clicks at the HUD button's screen position; the pause screen has no element there, so `fired` is empty purely because input is routed to the top screen only. This correctly proves "input does not reach the screen beneath," which is the modal contract.
