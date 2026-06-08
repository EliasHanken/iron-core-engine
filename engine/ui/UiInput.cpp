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

// M63: draggable/droppable/scrollbox collectors.
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

}  // namespace

UiInputResult updateUi(const UiElement& root, const UiLayoutMap& rects,
                       const UiInputState& in, UiId prevFocused,
                       const UiDragState& prevDrag,
                       const UiClipMap& clips) {
    UiInputResult res;
    res.drag = prevDrag;

    std::vector<Focusable> buttons;
    collect(root, buttons);

    // Button-specific hover/focus/nav/click — only when buttons exist.
    if (!buttons.empty()) {
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
    }

    // ---- M63: drag / drop / double-click / wheel (runs unconditionally) ----
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

    return res;
}

}  // namespace iron
