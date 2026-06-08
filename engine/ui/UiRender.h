#pragma once

#include "render/HudBatch.h"
#include "render/Handles.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiLayout.h"
#include "ui/UiInput.h"

namespace iron {

// Clip an axis-aligned quad to `clip`, remapping UVs proportionally. Mutates the
// passed min/max/uv in place. Returns false if the quad is fully outside.
bool uiClipQuad(Vec2& min, Vec2& max, Vec2& uvMin, Vec2& uvMax, const Rect& clip);

// Build screen-space HUD geometry for a laid-out tree. Panels/Bars/Buttons emit
// colored quads (via `whiteTexture`); Images use their own texture; Labels and
// Buttons draw text via `atlas`. The `hovered`/`focused` ids get a brighter
// overlay on Buttons. Hidden elements (and their subtrees) are skipped.
// The optional `clips` map clips elements inside scrolled ScrollBoxes.
HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused, const UiClipMap& clips = {});

// Render the dragged element's subtree translated so its top-left follows
// (cursor - drag.grabOffset), semi-transparent, as an on-top overlay. Returns an
// empty batch if !drag.active or the source id isn't in `rects`.
HudBatch renderUiDragGhost(const UiElement& root, const UiLayoutMap& rects,
                           const FontAtlas& atlas, TextureHandle whiteTexture,
                           const UiDragState& drag, Vec2 cursor);

}  // namespace iron
