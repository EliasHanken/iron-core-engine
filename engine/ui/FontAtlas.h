#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

#include <string>
#include <vector>

namespace iron {

// One laid-out glyph: pixel rect (in text-local space, baseline-relative) and
// atlas UVs. A whitespace glyph has min == max (nothing to draw) but still
// advances the pen.
struct GlyphQuad { Vec2 min, max, uvMin, uvMax; };

// A proportional font atlas baked from a TTF at one pixel height via
// stb_truetype. Holds CPU-side RGBA8 pixels (white, alpha = coverage) plus
// per-glyph metrics for printable ASCII (32..126). The caller uploads `pixels()`
// through Renderer::createTexture (srgb=false) and assigns the handle to
// `texture`.
class FontAtlas {
public:
    bool bake(const unsigned char* ttf, int ttfLen, float pixelHeight);

    // Append the quad for `c` and advance the pen (in baked-pixel space, with the
    // baseline at penY). Unknown glyphs leave the pen unchanged.
    GlyphQuad quadFor(char c, float& penX, float& penY) const;
    float textWidth(const std::string& text) const;   // baked-size advance width
    float pixelHeight() const { return pixelHeight_; }

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<unsigned char>& pixels() const { return pixels_; }  // RGBA8

    TextureHandle texture = kInvalidHandle;   // set by the caller after upload

private:
    static constexpr int kFirst = 32;
    static constexpr int kCount = 95;         // 32..126 inclusive

    struct Glyph {
        int   x0 = 0, y0 = 0, x1 = 0, y1 = 0; // atlas pixel box
        float xoff = 0, yoff = 0, xadvance = 0;
    };

    std::vector<Glyph> glyphs_;               // size kCount
    int   width_ = 0, height_ = 0;
    float pixelHeight_ = 0.0f;
    std::vector<unsigned char> pixels_;
};

}  // namespace iron
