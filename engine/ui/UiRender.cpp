#include "ui/UiRender.h"

namespace iron {

namespace {

void appendQuad(std::vector<HudVertex>& out, Vec2 min, Vec2 max,
                Vec2 uvMin, Vec2 uvMax, Vec4 color) {
    const HudVertex tl{Vec2{min.x, min.y}, Vec2{uvMin.x, uvMin.y}, color};
    const HudVertex tr{Vec2{max.x, min.y}, Vec2{uvMax.x, uvMin.y}, color};
    const HudVertex br{Vec2{max.x, max.y}, Vec2{uvMax.x, uvMax.y}, color};
    const HudVertex bl{Vec2{min.x, max.y}, Vec2{uvMin.x, uvMax.y}, color};
    out.push_back(tl); out.push_back(bl); out.push_back(br);
    out.push_back(tl); out.push_back(br); out.push_back(tr);
}

std::vector<HudVertex>& groupFor(HudBatch& batch, TextureHandle tex) {
    for (HudDrawGroup& g : batch)
        if (g.texture == tex) return g.vertices;
    batch.push_back(HudDrawGroup{tex, {}});
    return batch.back().vertices;
}

// Draw text with its top-left at `topLeft`, scaled so the em-height is `fontPx`.
void appendText(HudBatch& batch, const FontAtlas& atlas, const std::string& text,
                Vec2 topLeft, float fontPx, Vec4 color) {
    if (atlas.texture == kInvalidHandle || atlas.pixelHeight() <= 0.0f) return;
    const float scale = fontPx / atlas.pixelHeight();
    // Baseline ~0.8 em below the top so glyphs sit inside the rect.
    const Vec2 origin{topLeft.x, topLeft.y + fontPx * 0.8f};
    std::vector<HudVertex>& verts = groupFor(batch, atlas.texture);
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) {
        const GlyphQuad q = atlas.quadFor(c, penX, penY);
        if (q.min.x == q.max.x || q.min.y == q.max.y) continue;  // whitespace
        appendQuad(verts, origin + q.min * scale, origin + q.max * scale,
                   q.uvMin, q.uvMax, color);
    }
}

void renderRec(const UiElement& e, const UiLayoutMap& rects, const FontAtlas& atlas,
               TextureHandle white, UiId hovered, UiId focused, HudBatch& batch) {
    if (!e.visible) return;
    const auto it = rects.find(e.id);
    if (it == rects.end()) { for (const auto& c : e.children) renderRec(c, rects, atlas, white, hovered, focused, batch); return; }
    const Rect r = it->second;

    switch (e.kind) {
        case UiKind::Panel:
            if (e.color.w > 0.0f)
                appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        case UiKind::Image:
            appendQuad(groupFor(batch, e.texture), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        case UiKind::Bar: {
            appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, e.trackColor);
            const float frac = e.value < 0.0f ? 0.0f : (e.value > 1.0f ? 1.0f : e.value);
            const Vec2 fillMax{r.min.x + rectSize(r).x * frac, r.max.y};
            if (frac > 0.0f)
                appendQuad(groupFor(batch, white), r.min, fillMax, Vec2{0, 0}, Vec2{1, 1}, e.color);
            break;
        }
        case UiKind::Button: {
            Vec4 bg = e.color;
            if (e.id == hovered || e.id == focused) {   // brighten when active
                bg.x = bg.x + (1.0f - bg.x) * 0.25f;
                bg.y = bg.y + (1.0f - bg.y) * 0.25f;
                bg.z = bg.z + (1.0f - bg.z) * 0.25f;
            }
            appendQuad(groupFor(batch, white), r.min, r.max, Vec2{0, 0}, Vec2{1, 1}, bg);
            // Centered caption.
            const float w = atlas.textWidth(e.text) * (e.fontPx / atlas.pixelHeight());
            const Vec2 c = rectCenter(r);
            appendText(batch, atlas, e.text, Vec2{c.x - w * 0.5f, c.y - e.fontPx * 0.5f},
                       e.fontPx, Vec4{1, 1, 1, 1});
            break;
        }
        case UiKind::Label:
            appendText(batch, atlas, e.text, r.min, e.fontPx, e.color);
            break;
    }

    for (const UiElement& c : e.children)
        renderRec(c, rects, atlas, white, hovered, focused, batch);
}

}  // namespace

HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused) {
    HudBatch batch;
    renderRec(root, rects, atlas, whiteTexture, hovered, focused, batch);
    return batch;
}

}  // namespace iron
