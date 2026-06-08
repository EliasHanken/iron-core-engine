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

enum class UiKind { Panel, Label, Image, Button, Bar, Grid, ScrollBox };

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

    int           gridCols = 0;                // Grid: columns per row
    float         scrollOffset = 0.0f;         // ScrollBox: current vertical scroll (px)
    Vec4          nineSliceMargin{0, 0, 0, 0}; // L,T,R,B dest border px (0 => plain quad)
    float         nineSliceUv = 0.0f;          // source border fraction 0..1 (uniform)
    bool          draggable = false;           // can initiate a drag
    bool          dropTarget = false;          // can receive a drop
    std::uint32_t userData = 0;                // payload (game encodes container/slot)

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
    if (a.gridCols != b.gridCols) return false;
    if (!nf(a.scrollOffset, b.scrollOffset)) return false;
    if (!n4(a.nineSliceMargin, b.nineSliceMargin) || !nf(a.nineSliceUv, b.nineSliceUv)) return false;
    if (a.draggable != b.draggable || a.dropTarget != b.dropTarget) return false;
    if (a.userData != b.userData) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i)
        if (!uiEqual(a.children[i], b.children[i])) return false;
    return true;
}

}  // namespace iron
