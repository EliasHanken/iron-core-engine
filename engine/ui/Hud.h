#pragma once

#include "math/Vec.h"
#include "render/Handles.h"
#include "render/HudBatch.h"
#include "ui/BitmapFont.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// A handle to a retained HUD element. 0 is invalid; valid ids are 1-based.
using HudId = std::uint32_t;

// A retained-mode screen-space HUD. The game adds elements once, keeps their
// ids, and mutates them by id. build() turns the visible elements into a
// HudBatch of screen-space quads. Coordinates are pixels, origin top-left.
class Hud {
public:
    // Adds an element; returns its id. `color` is the text/panel colour or the
    // image tint.
    HudId addText(std::string text, Vec2 position, float scale, Vec4 color);
    HudId addPanel(Vec2 position, Vec2 size, Vec4 color);
    HudId addImage(Vec2 position, Vec2 size, TextureHandle texture, Vec4 tint);

    // Mutators. An out-of-range id is ignored.
    void setText(HudId id, std::string text);
    void setPosition(HudId id, Vec2 position);
    void setColor(HudId id, Vec4 color);
    void setSize(HudId id, Vec2 size);
    void setVisible(HudId id, bool visible);

    // Builds the screen-space quad batch for the current visible elements.
    // Text quads use `font.atlas`; panels use `whiteTexture`; images use their
    // own texture. Quads are grouped by texture.
    HudBatch build(const BitmapFont& font, TextureHandle whiteTexture) const;

private:
    enum class Kind { Text, Panel, Image };

    struct Element {
        Kind kind = Kind::Panel;
        Vec2 position;
        Vec2 size;                              // Panel / Image
        Vec4 color;                             // colour or image tint
        std::string text;                       // Text
        float scale = 1.0f;                     // Text
        TextureHandle texture = kInvalidHandle;  // Image
        bool visible = true;
    };

    Element* get(HudId id);

    std::vector<Element> elements_;
};

} // namespace iron
