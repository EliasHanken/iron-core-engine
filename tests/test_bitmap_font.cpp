#include "test_framework.h"
#include "ui/BitmapFont.h"

using namespace iron;

int main() {
    // A 16x16 grid: glyph 0 is the top-left cell, spanning one 1/16 cell.
    {
        BitmapFont font;  // defaults: 16 cols, 16 rows, 8x8 glyphs
        const GlyphUv g = glyphUv(font, 0);
        CHECK_NEAR(g.min.x, 0.0f);
        CHECK_NEAR(g.min.y, 0.0f);
        CHECK_NEAR(g.max.x, 1.0f / 16.0f);
        CHECK_NEAR(g.max.y, 1.0f / 16.0f);
    }

    // 'A' is code 65 -> cell (65 % 16, 65 / 16) = (1, 4).
    {
        BitmapFont font;
        const GlyphUv g = glyphUv(font, 'A');
        CHECK_NEAR(g.min.x, 1.0f / 16.0f);
        CHECK_NEAR(g.min.y, 4.0f / 16.0f);
        CHECK_NEAR(g.max.x, 2.0f / 16.0f);
        CHECK_NEAR(g.max.y, 5.0f / 16.0f);
    }

    // Code 16 wraps to the start of the second row.
    {
        BitmapFont font;
        const GlyphUv g = glyphUv(font, 16);
        CHECK_NEAR(g.min.x, 0.0f);
        CHECK_NEAR(g.min.y, 1.0f / 16.0f);
        CHECK_NEAR(g.max.x, 1.0f / 16.0f);
        CHECK_NEAR(g.max.y, 2.0f / 16.0f);
    }

    return iron_test_result();
}
