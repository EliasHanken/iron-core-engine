#pragma once

// Text with wide letter spacing, for titles over a demo.

#include "core/Platform.h"
#include "render/HudBatch.h"
#include "render/Renderer.h"
#include "ui/FontAtlas.h"

#include <cstdio>
#include <string>
#include <vector>

namespace text {

// Bakes a font next to the exe, and uploads it.
inline bool loadFont(iron::Renderer& renderer, iron::FontAtlas& atlas, const std::string& relativePath,
                     float pixels) {
    const std::string path = iron::executableDir() + "/" + relativePath;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    if (read != bytes.size() ||
        !atlas.bake(bytes.data(), static_cast<int>(bytes.size()), pixels)) {
        return false;
    }
    atlas.texture = renderer.createTexture(atlas.width(), atlas.height(), atlas.pixels().data(),
                                           /*srgb=*/false);
    return true;
}

// Centred text with extra letter spacing, `tracking` in em.
inline void appendCentered(iron::HudDrawGroup& group, const iron::FontAtlas& atlas,
                           const std::string& text, float centerX, float baseline, float scale,
                           float tracking, iron::Vec4 color) {
    const float gap = atlas.pixelHeight() * tracking;
    const float width = atlas.textWidth(text) + gap * static_cast<float>(text.size() - 1);

    float penX = 0.0f;
    float penY = 0.0f;
    const float left = centerX - width * scale * 0.5f;
    for (char c : text) {
        const iron::GlyphQuad q = atlas.quadFor(c, penX, penY);
        penX += gap;
        if (q.min.x == q.max.x) continue;

        const iron::Vec2 a{left + q.min.x * scale, baseline + q.min.y * scale};
        const iron::Vec2 b{left + q.max.x * scale, baseline + q.max.y * scale};
        const iron::HudVertex corners[4] = {
            {{a.x, a.y}, {q.uvMin.x, q.uvMin.y}, color},
            {{b.x, a.y}, {q.uvMax.x, q.uvMin.y}, color},
            {{b.x, b.y}, {q.uvMax.x, q.uvMax.y}, color},
            {{a.x, b.y}, {q.uvMin.x, q.uvMax.y}, color},
        };
        for (int i : {0, 1, 2, 0, 2, 3}) group.vertices.push_back(corners[i]);
    }
}

}  // namespace text
