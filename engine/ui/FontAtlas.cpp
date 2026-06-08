#include "ui/FontAtlas.h"

#include <stb_truetype.h>   // declarations; implementation is in third_party/stb

namespace iron {

bool FontAtlas::bake(const unsigned char* ttf, int ttfLen, float pixelHeight) {
    if (!ttf || ttfLen <= 0 || pixelHeight <= 0.0f) return false;

    width_ = 512;
    height_ = 512;
    pixelHeight_ = pixelHeight;

    std::vector<unsigned char> coverage(static_cast<std::size_t>(width_) * height_, 0);
    std::vector<stbtt_bakedchar> cd(kCount);
    const int rows = stbtt_BakeFontBitmap(ttf, 0, pixelHeight, coverage.data(),
                                          width_, height_, kFirst, kCount, cd.data());
    if (rows == 0) return false;   // not a single row fit -> bad font / size

    // Expand 8-bit coverage to RGBA8 white (tinted later by vertex color).
    pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        pixels_[i * 4 + 0] = 255;
        pixels_[i * 4 + 1] = 255;
        pixels_[i * 4 + 2] = 255;
        pixels_[i * 4 + 3] = coverage[i];
    }

    glyphs_.resize(kCount);
    for (int i = 0; i < kCount; ++i) {
        const stbtt_bakedchar& b = cd[i];
        Glyph g;
        g.x0 = b.x0; g.y0 = b.y0; g.x1 = b.x1; g.y1 = b.y1;
        g.xoff = b.xoff; g.yoff = b.yoff; g.xadvance = b.xadvance;
        glyphs_[i] = g;
    }
    return true;
}

GlyphQuad FontAtlas::quadFor(char c, float& penX, float& penY) const {
    GlyphQuad q{};
    const int idx = static_cast<unsigned char>(c) - kFirst;
    if (idx < 0 || idx >= kCount || glyphs_.empty()) return q;

    const Glyph& g = glyphs_[static_cast<std::size_t>(idx)];
    const float ipw = 1.0f / static_cast<float>(width_);
    const float iph = 1.0f / static_cast<float>(height_);

    const float rx = penX + g.xoff;
    const float ry = penY + g.yoff;
    q.min   = Vec2{rx, ry};
    q.max   = Vec2{rx + static_cast<float>(g.x1 - g.x0), ry + static_cast<float>(g.y1 - g.y0)};
    q.uvMin = Vec2{static_cast<float>(g.x0) * ipw, static_cast<float>(g.y0) * iph};
    q.uvMax = Vec2{static_cast<float>(g.x1) * ipw, static_cast<float>(g.y1) * iph};

    penX += g.xadvance;
    return q;
}

float FontAtlas::textWidth(const std::string& text) const {
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) quadFor(c, penX, penY);
    return penX;
}

}  // namespace iron
