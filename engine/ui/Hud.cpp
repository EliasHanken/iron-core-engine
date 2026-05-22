#include "ui/Hud.h"

#include <utility>

namespace iron {

namespace {

// Appends one quad (two triangles, 6 vertices) spanning the pixel rectangle
// `min`..`max` with UV rectangle `uvMin`..`uvMax`, all vertices `color`.
// Winding is CCW in NDC after the HUD shader's y-flip.
void appendQuad(std::vector<HudVertex>& out, Vec2 min, Vec2 max,
                Vec2 uvMin, Vec2 uvMax, Vec4 color) {
    const HudVertex tl{Vec2{min.x, min.y}, Vec2{uvMin.x, uvMin.y}, color};
    const HudVertex tr{Vec2{max.x, min.y}, Vec2{uvMax.x, uvMin.y}, color};
    const HudVertex br{Vec2{max.x, max.y}, Vec2{uvMax.x, uvMax.y}, color};
    const HudVertex bl{Vec2{min.x, max.y}, Vec2{uvMin.x, uvMax.y}, color};
    out.push_back(tl);
    out.push_back(bl);
    out.push_back(br);
    out.push_back(tl);
    out.push_back(br);
    out.push_back(tr);
}

}  // namespace

HudId Hud::addText(std::string text, Vec2 position, float scale, Vec4 color) {
    Element e;
    e.kind = Kind::Text;
    e.position = position;
    e.scale = scale;
    e.color = color;
    e.text = std::move(text);
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

HudId Hud::addPanel(Vec2 position, Vec2 size, Vec4 color) {
    Element e;
    e.kind = Kind::Panel;
    e.position = position;
    e.size = size;
    e.color = color;
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

HudId Hud::addImage(Vec2 position, Vec2 size, TextureHandle texture,
                    Vec4 tint) {
    Element e;
    e.kind = Kind::Image;
    e.position = position;
    e.size = size;
    e.color = tint;
    e.texture = texture;
    elements_.push_back(std::move(e));
    return static_cast<HudId>(elements_.size());
}

Hud::Element* Hud::get(HudId id) {
    if (id == 0 || id > elements_.size()) {
        return nullptr;
    }
    return &elements_[id - 1];
}

void Hud::setText(HudId id, std::string text) {
    if (Element* e = get(id)) {
        e->text = std::move(text);
    }
}

void Hud::setPosition(HudId id, Vec2 position) {
    if (Element* e = get(id)) {
        e->position = position;
    }
}

void Hud::setColor(HudId id, Vec4 color) {
    if (Element* e = get(id)) {
        e->color = color;
    }
}

void Hud::setVisible(HudId id, bool visible) {
    if (Element* e = get(id)) {
        e->visible = visible;
    }
}

HudBatch Hud::build(const BitmapFont& font, TextureHandle whiteTexture) const {
    HudBatch batch;

    // Find (or create) the vertex list for a texture's draw group.
    auto groupFor = [&batch](TextureHandle tex) -> std::vector<HudVertex>& {
        for (HudDrawGroup& g : batch) {
            if (g.texture == tex) {
                return g.vertices;
            }
        }
        batch.push_back(HudDrawGroup{tex, {}});
        return batch.back().vertices;
    };

    for (const Element& e : elements_) {
        if (!e.visible) {
            continue;
        }
        switch (e.kind) {
            case Kind::Panel:
                appendQuad(groupFor(whiteTexture), e.position,
                           e.position + e.size, Vec2{0, 0}, Vec2{1, 1},
                           e.color);
                break;
            case Kind::Image:
                appendQuad(groupFor(e.texture), e.position,
                           e.position + e.size, Vec2{0, 0}, Vec2{1, 1},
                           e.color);
                break;
            case Kind::Text: {
                // groupFor is called once here; calling it again before this
                // reference is done would risk a batch.push_back reallocating
                // the draw groups and dangling `verts`.
                std::vector<HudVertex>& verts = groupFor(font.atlas);
                const float gw =
                    static_cast<float>(font.glyphPixelWidth) * e.scale;
                const float gh =
                    static_cast<float>(font.glyphPixelHeight) * e.scale;
                float penX = e.position.x;
                float penY = e.position.y;
                for (char ch : e.text) {
                    if (ch == '\n') {
                        penX = e.position.x;
                        penY += gh;
                        continue;
                    }
                    const GlyphUv g =
                        glyphUv(font, static_cast<unsigned char>(ch));
                    appendQuad(verts, Vec2{penX, penY},
                               Vec2{penX + gw, penY + gh}, g.min, g.max,
                               e.color);
                    penX += gw;
                }
                break;
            }
        }
    }
    return batch;
}

} // namespace iron
