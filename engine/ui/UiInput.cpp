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
