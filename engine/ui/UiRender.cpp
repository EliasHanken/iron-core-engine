#include "ui/UiRender.h"

#include <algorithm>

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

bool clipQuadImpl(Vec2& mn, Vec2& mx, Vec2& uvMin, Vec2& uvMax, const Rect& clip) {
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    if (w <= 0.0f || h <= 0.0f) return false;
    const float nx0 = std::max(mn.x, clip.min.x);
    const float ny0 = std::max(mn.y, clip.min.y);
    const float nx1 = std::min(mx.x, clip.max.x);
    const float ny1 = std::min(mx.y, clip.max.y);
    if (nx1 <= nx0 || ny1 <= ny0) return false;
    const float uw = uvMax.x - uvMin.x, uh = uvMax.y - uvMin.y;
    const Vec2 nUvMin{uvMin.x + (nx0 - mn.x) / w * uw, uvMin.y + (ny0 - mn.y) / h * uh};
    const Vec2 nUvMax{uvMin.x + (nx1 - mn.x) / w * uw, uvMin.y + (ny1 - mn.y) / h * uh};
    mn = Vec2{nx0, ny0}; mx = Vec2{nx1, ny1};
    uvMin = nUvMin; uvMax = nUvMax;
    return true;
}

// appendQuad that clips first when `clip` is non-null.
void appendQuadClipped(std::vector<HudVertex>& out, Vec2 mn, Vec2 mx,
                       Vec2 uvMin, Vec2 uvMax, Vec4 color, const Rect* clip) {
    if (clip) { if (!clipQuadImpl(mn, mx, uvMin, uvMax, *clip)) return; }
    appendQuad(out, mn, mx, uvMin, uvMax, color);
}

// Emit a 9-slice expansion of [r] using margin (L,T,R,B px) and uniform source
// border fraction `uvb`, into `verts`, clipped by `clip` if non-null.
void appendNineSlice(std::vector<HudVertex>& verts, const Rect& r, Vec4 m,
                     float uvb, Vec4 color, const Rect* clip) {
    // Clamp inner split points so the corner/edge cells never overshoot the
    // rect when a margin is larger than the element (xs/ys stay monotonic within
    // [min,max]; an oversized margin collapses the center cell instead of
    // producing inverted, out-of-bounds quads).
    const float x1 = std::min(r.min.x + m.x, r.max.x);
    const float x2 = std::max(r.max.x - m.z, x1);
    const float y1 = std::min(r.min.y + m.y, r.max.y);
    const float y2 = std::max(r.max.y - m.w, y1);
    const float xs[4] = {r.min.x, x1, x2, r.max.x};
    const float ys[4] = {r.min.y, y1, y2, r.max.y};
    const float us[4] = {0.0f, uvb, 1.0f - uvb, 1.0f};
    const float vs[4] = {0.0f, uvb, 1.0f - uvb, 1.0f};
    for (int row = 0; row < 3; ++row) {
        for (int colc = 0; colc < 3; ++colc) {
            const Vec2 mn{xs[colc], ys[row]}, mx{xs[colc + 1], ys[row + 1]};
            if (mx.x <= mn.x || mx.y <= mn.y) continue;     // degenerate cell
            appendQuadClipped(verts, mn, mx, Vec2{us[colc], vs[row]},
                              Vec2{us[colc + 1], vs[row + 1]}, color, clip);
        }
    }
}

// Draw text with its top-left at `topLeft`, scaled so the em-height is `fontPx`.
void appendText(HudBatch& batch, const FontAtlas& atlas, const std::string& text,
                Vec2 topLeft, float fontPx, Vec4 color, const Rect* clip) {
    if (atlas.texture == kInvalidHandle || atlas.pixelHeight() <= 0.0f) return;
    const float scale = fontPx / atlas.pixelHeight();
    // Baseline ~0.8 em below the top so glyphs sit inside the rect.
    const Vec2 origin{topLeft.x, topLeft.y + fontPx * 0.8f};
    std::vector<HudVertex>& verts = groupFor(batch, atlas.texture);
    float penX = 0.0f, penY = 0.0f;
    for (char c : text) {
        const GlyphQuad q = atlas.quadFor(c, penX, penY);
        if (q.min.x == q.max.x || q.min.y == q.max.y) continue;  // whitespace
        appendQuadClipped(verts, origin + q.min * scale, origin + q.max * scale,
                          q.uvMin, q.uvMax, color, clip);
    }
}

void renderRec(const UiElement& e, const UiLayoutMap& rects, const FontAtlas& atlas,
               TextureHandle white, UiId hovered, UiId focused,
               const UiClipMap& clips, HudBatch& batch) {
    if (!e.visible) return;
    const auto it = rects.find(e.id);
    if (it == rects.end()) {
        for (const auto& c : e.children)
            renderRec(c, rects, atlas, white, hovered, focused, clips, batch);
        return;
    }
    const Rect r = it->second;

    const auto clipIt = clips.find(e.id);
    const Rect* clip = (clipIt != clips.end()) ? &clipIt->second : nullptr;

    switch (e.kind) {
        case UiKind::Panel:
            if (e.color.w > 0.0f)
                appendQuadClipped(groupFor(batch, white), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
        case UiKind::Image:
            if (e.nineSliceMargin.x != 0.0f || e.nineSliceMargin.y != 0.0f ||
                e.nineSliceMargin.z != 0.0f || e.nineSliceMargin.w != 0.0f)
                appendNineSlice(groupFor(batch, e.texture), r, e.nineSliceMargin,
                                e.nineSliceUv, e.color, clip);
            else
                appendQuadClipped(groupFor(batch, e.texture), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
        case UiKind::Bar: {
            appendQuadClipped(groupFor(batch, white), r.min, r.max,
                              Vec2{0, 0}, Vec2{1, 1}, e.trackColor, clip);
            const float frac = e.value < 0.0f ? 0.0f : (e.value > 1.0f ? 1.0f : e.value);
            const Vec2 fillMax{r.min.x + rectSize(r).x * frac, r.max.y};
            if (frac > 0.0f)
                appendQuadClipped(groupFor(batch, white), r.min, fillMax,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
        }
        case UiKind::Button: {
            Vec4 bg = e.color;
            if (e.id == hovered || e.id == focused) {   // brighten when active
                bg.x = bg.x + (1.0f - bg.x) * 0.25f;
                bg.y = bg.y + (1.0f - bg.y) * 0.25f;
                bg.z = bg.z + (1.0f - bg.z) * 0.25f;
            }
            appendQuadClipped(groupFor(batch, white), r.min, r.max,
                              Vec2{0, 0}, Vec2{1, 1}, bg, clip);
            // Centered caption.
            const float w = atlas.textWidth(e.text) * (e.fontPx / atlas.pixelHeight());
            const Vec2 c = rectCenter(r);
            appendText(batch, atlas, e.text, Vec2{c.x - w * 0.5f, c.y - e.fontPx * 0.5f},
                       e.fontPx, Vec4{1, 1, 1, 1}, clip);
            break;
        }
        case UiKind::Label:
            appendText(batch, atlas, e.text, r.min, e.fontPx, e.color, clip);
            break;
        case UiKind::Grid:
        case UiKind::ScrollBox:
            if (e.color.w > 0.0f)
                appendQuadClipped(groupFor(batch, white), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, e.color, clip);
            break;
    }

    for (const UiElement& c : e.children)
        renderRec(c, rects, atlas, white, hovered, focused, clips, batch);
}

}  // namespace

bool uiClipQuad(Vec2& min, Vec2& max, Vec2& uvMin, Vec2& uvMax, const Rect& clip) {
    return clipQuadImpl(min, max, uvMin, uvMax, clip);
}

HudBatch renderUi(const UiElement& root, const UiLayoutMap& rects,
                  const FontAtlas& atlas, TextureHandle whiteTexture,
                  UiId hovered, UiId focused, const UiClipMap& clips) {
    HudBatch batch;
    renderRec(root, rects, atlas, whiteTexture, hovered, focused, clips, batch);
    return batch;
}

namespace {
// Find element by id (depth-first).
const UiElement* findById(const UiElement& e, UiId id) {
    if (e.id == id) return &e;
    for (const UiElement& c : e.children) if (const UiElement* f = findById(c, id)) return f;
    return nullptr;
}
// Re-render a subtree with all rects translated by `delta`, alpha-scaled.
void renderGhostRec(const UiElement& e, const UiLayoutMap& rects, const FontAtlas& atlas,
                    TextureHandle white, Vec2 delta, float alpha, HudBatch& batch) {
    if (!e.visible) return;
    const auto it = rects.find(e.id);
    if (it != rects.end()) {
        const Rect r{it->second.min + delta, it->second.max + delta};
        Vec4 col = e.color; col.w *= alpha;
        if (e.kind == UiKind::Image) {
            if (e.nineSliceMargin.x != 0.0f || e.nineSliceMargin.y != 0.0f ||
                e.nineSliceMargin.z != 0.0f || e.nineSliceMargin.w != 0.0f)
                appendNineSlice(groupFor(batch, e.texture), r, e.nineSliceMargin,
                                e.nineSliceUv, col, nullptr);
            else
                appendQuadClipped(groupFor(batch, e.texture), r.min, r.max,
                                  Vec2{0, 0}, Vec2{1, 1}, col, nullptr);
        } else if (e.kind == UiKind::Label) {
            appendText(batch, atlas, e.text, r.min, e.fontPx, col, nullptr);
        } else if (e.color.w > 0.0f) {
            appendQuadClipped(groupFor(batch, white), r.min, r.max,
                              Vec2{0, 0}, Vec2{1, 1}, col, nullptr);
        }
    }
    for (const UiElement& c : e.children)
        renderGhostRec(c, rects, atlas, white, delta, alpha, batch);
}
}  // namespace

HudBatch renderUiDragGhost(const UiElement& root, const UiLayoutMap& rects,
                           const FontAtlas& atlas, TextureHandle whiteTexture,
                           const UiDragState& drag, Vec2 cursor) {
    HudBatch batch;
    if (!drag.active) return batch;
    const UiElement* src = findById(root, drag.sourceId);
    if (!src) return batch;
    const auto it = rects.find(drag.sourceId);
    if (it == rects.end()) return batch;
    const Vec2 newMin{cursor.x - drag.grabOffset.x, cursor.y - drag.grabOffset.y};
    const Vec2 delta{newMin.x - it->second.min.x, newMin.y - it->second.min.y};
    renderGhostRec(*src, rects, atlas, whiteTexture, delta, 0.75f, batch);
    return batch;
}

}  // namespace iron
