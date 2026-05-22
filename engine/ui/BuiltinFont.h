#pragma once

#include "render/Handles.h"
#include "ui/BitmapFont.h"

#include <vector>

namespace iron {

// A CPU-side RGBA atlas image: `width * height * 4` bytes, row-major from the
// top-left. Upload it with Renderer::createTexture.
struct BuiltinFontAtlas {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

// Rasterizes the embedded public-domain 8x8 font into a 128x128 RGBA atlas: a
// 16x16 grid of 8x8-pixel cells. A set glyph pixel is opaque white; a clear
// pixel is transparent black. Codes 128-255 are blank cells.
BuiltinFontAtlas builtinFontAtlas();

// The BitmapFont metrics matching builtinFontAtlas(), bound to texture `atlas`.
BitmapFont builtinFont(TextureHandle atlas);

} // namespace iron
