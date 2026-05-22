#include "test_framework.h"
#include "ui/BitmapFont.h"
#include "ui/BuiltinFont.h"

#include <cstddef>

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

    // The atlas is 128x128 RGBA.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        CHECK(atlas.width == 128);
        CHECK(atlas.height == 128);
        CHECK(atlas.rgba.size() == 128u * 128u * 4u);
    }

    // Every pixel is either transparent black or opaque white.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        for (std::size_t i = 0; i < atlas.rgba.size(); i += 4) {
            const unsigned char r = atlas.rgba[i + 0];
            const unsigned char g = atlas.rgba[i + 1];
            const unsigned char b = atlas.rgba[i + 2];
            const unsigned char a = atlas.rgba[i + 3];
            const bool clear = (r == 0 && g == 0 && b == 0 && a == 0);
            const bool white = (r == 255 && g == 255 && b == 255 && a == 255);
            CHECK(clear || white);
        }
    }

    // Glyph 'A' (65) has at least one set pixel; the space glyph (32) is blank.
    {
        const BuiltinFontAtlas atlas = builtinFontAtlas();
        auto cellHasInk = [&atlas](int code) {
            const int cellX = (code % 16) * 8;
            const int cellY = (code / 16) * 8;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    const std::size_t i =
                        (static_cast<std::size_t>(cellY + y) * 128
                         + (cellX + x)) * 4;
                    if (atlas.rgba[i + 3] != 0) return true;
                }
            }
            return false;
        };
        CHECK(cellHasInk('A'));
        CHECK(!cellHasInk(' '));
    }

    // builtinFont reports the matching grid metrics.
    {
        const BitmapFont font = builtinFont(7);
        CHECK(font.atlas == 7);
        CHECK(font.columns == 16);
        CHECK(font.rows == 16);
        CHECK(font.glyphPixelWidth == 8);
        CHECK(font.glyphPixelHeight == 8);
    }

    return iron_test_result();
}
