#pragma once

#include "render/HudBatch.h"
#include "render/Handles.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiLayout.h"

namespace iron {

// Build screen-space HUD geometry for a laid-out tree. Panels/Bars/Buttons emit
// colored quads (via `whiteTexture`); Images use their own texture; Labels and
// Buttons draw text via `atlas`. The `hovered`/`focused` ids get a brighter
// overlay on Buttons. Hidden elements (and their subtrees) are skipped.
HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused);

}  // namespace iron
