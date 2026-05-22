#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

namespace iron {

// Describes a fixed-grid monospace bitmap font: an atlas texture whose glyphs
// are laid out in a `columns` x `rows` grid of equal cells. Character code `c`
// occupies cell (c % columns, c / columns).
struct BitmapFont {
    TextureHandle atlas = kInvalidHandle;
    int columns = 16;
    int rows = 16;
    int glyphPixelWidth = 8;
    int glyphPixelHeight = 8;
};

// The UV rectangle of one glyph cell within the atlas. `min` is the top-left
// corner, `max` the bottom-right.
struct GlyphUv {
    Vec2 min;
    Vec2 max;
};

// Returns the UV rectangle of character `c` in `font`'s atlas grid.
GlyphUv glyphUv(const BitmapFont& font, unsigned char c);

} // namespace iron
