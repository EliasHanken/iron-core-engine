#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

#include <vector>

namespace iron {

// One vertex of a screen-space HUD quad. `position` is in pixels with the
// origin at the top-left of the framebuffer (x right, y down).
struct HudVertex {
    Vec2 position;
    Vec2 uv;
    Vec4 color;
};

// All HUD quads that share one texture, as a triangle list (6 vertices per
// quad — HUD geometry is small and rebuilt every frame, so no index buffer).
struct HudDrawGroup {
    TextureHandle texture = kInvalidHandle;
    std::vector<HudVertex> vertices;
};

// A whole frame's HUD geometry, grouped by texture.
using HudBatch = std::vector<HudDrawGroup>;

} // namespace iron
