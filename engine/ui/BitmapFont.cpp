#include "ui/BitmapFont.h"

namespace iron {

GlyphUv glyphUv(const BitmapFont& font, unsigned char c) {
    const int col = c % font.columns;
    const int row = c / font.columns;
    const float colf = static_cast<float>(font.columns);
    const float rowf = static_cast<float>(font.rows);
    GlyphUv uv;
    uv.min = Vec2{static_cast<float>(col) / colf,
                  static_cast<float>(row) / rowf};
    uv.max = Vec2{static_cast<float>(col + 1) / colf,
                  static_cast<float>(row + 1) / rowf};
    return uv;
}

} // namespace iron
