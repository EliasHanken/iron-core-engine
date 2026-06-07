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
