#include "ui/UiLayout.h"

#include <algorithm>

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

}  // namespace iron
