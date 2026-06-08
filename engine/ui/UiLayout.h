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

// Clip rects for elements inside a scrolled ScrollBox (element id -> viewport rect).
using UiClipMap = std::unordered_map<UiId, Rect>;

// Resolve screen rects for `root` and every descendant. Anchors resolve against
// the parent rect; Stretch fills the parent inset by `offset`; Stack panels flow
// children along the stack axis (cross axis still honors each child's anchor).
// The tree must have ids assigned (uiAssignIds) — elements with id 0 are skipped.
UiLayoutMap layoutUi(const UiElement& root, Vec2 screenSize);

// Post-layout pass: for each ScrollBox in `root` with a non-zero offset in
// `offsets` (keyed by ScrollBox id), shift all its descendant rects up by the
// (clamped) offset and assign them a clip rect equal to the ScrollBox's rect.
// Mutates `rects` in place; returns the clip map. The offset is clamped to
// [0, contentHeight - viewportHeight] so stale/oversized values self-correct.
UiClipMap applyScroll(const UiElement& root, UiLayoutMap& rects,
                      const std::unordered_map<UiId, float>& offsets);

}  // namespace iron
