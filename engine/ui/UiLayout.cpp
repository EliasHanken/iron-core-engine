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
