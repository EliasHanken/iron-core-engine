#pragma once

#include "math/Vec.h"

namespace iron {

// Map a window-space (top-left origin) mouse position into a viewport panel's
// local pixel space. Returns true if the point is inside [rectMin, rectMin+size)
// and writes the rect-local position into `outLocal`; returns false otherwise
// (outLocal left unspecified). A zero-area rect is never "inside".
//
// Used by the editor to feed viewport-relative pixels to screenPointToRay and
// to gate camera/picking/gizmo interaction on the cursor being over the 3D viewport.
inline bool viewportLocalMouse(Vec2 mousePx, Vec2 rectMin, Vec2 rectSize,
                               Vec2& outLocal) {
    if (rectSize.x <= 0.0f || rectSize.y <= 0.0f) return false;
    const float lx = mousePx.x - rectMin.x;
    const float ly = mousePx.y - rectMin.y;
    if (lx < 0.0f || ly < 0.0f || lx >= rectSize.x || ly >= rectSize.y) return false;
    outLocal = Vec2{lx, ly};
    return true;
}

}  // namespace iron
