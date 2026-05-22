#include "test_framework.h"
#include "math/Vec.h"
#include "render/HudBatch.h"
#include "ui/BitmapFont.h"
#include "ui/Hud.h"

#include <cstddef>

using namespace iron;

namespace {
// A test font: 16x16 grid, 8x8 glyphs, atlas handle 2.
BitmapFont testFont() {
    BitmapFont f;
    f.atlas = 2;
    f.columns = 16;
    f.rows = 16;
    f.glyphPixelWidth = 8;
    f.glyphPixelHeight = 8;
    return f;
}

// Total vertices across every draw group in a batch.
std::size_t totalVertices(const HudBatch& batch) {
    std::size_t n = 0;
    for (const HudDrawGroup& g : batch) n += g.vertices.size();
    return n;
}

// The draw group for a given texture, or nullptr.
const HudDrawGroup* groupFor(const HudBatch& batch, TextureHandle tex) {
    for (const HudDrawGroup& g : batch) {
        if (g.texture == tex) return &g;
    }
    return nullptr;
}
}  // namespace

int main() {
    const TextureHandle kWhite = 1;
    const BitmapFont font = testFont();

    // add* returns distinct, non-zero ids.
    {
        Hud hud;
        const HudId a = hud.addPanel(Vec2{0, 0}, Vec2{10, 10}, Vec4{1,1,1,1});
        const HudId b = hud.addText("hi", Vec2{0, 0}, 1.0f, Vec4{1,1,1,1});
        const HudId c = hud.addImage(Vec2{0,0}, Vec2{4,4}, 9, Vec4{1,1,1,1});
        CHECK(a != 0 && b != 0 && c != 0);
        CHECK(a != b && b != c && a != c);
    }

    // A panel emits one quad (6 vertices) in the white-texture group.
    {
        Hud hud;
        hud.addPanel(Vec2{5, 5}, Vec2{20, 10}, Vec4{1, 0, 0, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, kWhite);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 6);
    }

    // An image's quad lands in its own texture's group.
    {
        Hud hud;
        hud.addImage(Vec2{0, 0}, Vec2{8, 8}, 9, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, 9);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 6);
    }

    // A 3-character label emits 3 quads (18 vertices) in the font-atlas group;
    // glyphs advance by glyphPixelWidth * scale along x.
    {
        Hud hud;
        hud.addText("ABC", Vec2{100, 50}, 1.0f, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, font.atlas);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 18);
        // First vertex of glyph 0 vs glyph 1: 6 vertices per quad.
        CHECK_NEAR(g->vertices[0].position.x, 100.0f);
        CHECK_NEAR(g->vertices[6].position.x, 108.0f);
        CHECK_NEAR(g->vertices[6].position.y, 50.0f);
    }

    // '\n' starts a new line: the glyph after it returns to the start x and
    // drops by one glyph height.
    {
        Hud hud;
        hud.addText("A\nB", Vec2{30, 40}, 1.0f, Vec4{1, 1, 1, 1});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, font.atlas);
        CHECK(g != nullptr);
        CHECK(g->vertices.size() == 12);  // 'A' and 'B', \n emits nothing
        // 'B' is the second quad: vertices [6..11].
        CHECK_NEAR(g->vertices[6].position.x, 30.0f);
        CHECK_NEAR(g->vertices[6].position.y, 48.0f);
    }

    // setVisible(false) drops an element from the batch.
    {
        Hud hud;
        const HudId p = hud.addPanel(Vec2{0,0}, Vec2{10,10}, Vec4{1,1,1,1});
        hud.setVisible(p, false);
        const HudBatch batch = hud.build(font, kWhite);
        CHECK(totalVertices(batch) == 0);
    }

    // setText to a longer string grows that element's quad count.
    {
        Hud hud;
        const HudId t = hud.addText("AB", Vec2{0,0}, 1.0f, Vec4{1,1,1,1});
        CHECK(totalVertices(hud.build(font, kWhite)) == 12);
        hud.setText(t, "ABCD");
        CHECK(totalVertices(hud.build(font, kWhite)) == 24);
    }

    // setPosition moves the element's quads.
    {
        Hud hud;
        const HudId p = hud.addPanel(Vec2{0, 0}, Vec2{10, 10}, Vec4{1,1,1,1});
        hud.setPosition(p, Vec2{50, 60});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, kWhite);
        CHECK(g != nullptr);
        CHECK_NEAR(g->vertices[0].position.x, 50.0f);
        CHECK_NEAR(g->vertices[0].position.y, 60.0f);
    }

    // Out-of-range ids are silently ignored by mutators.
    {
        Hud hud;
        hud.addPanel(Vec2{0, 0}, Vec2{10, 10}, Vec4{1,1,1,1});
        hud.setText(0, "x");        // id 0 is invalid
        hud.setVisible(99, false);  // id beyond range
        const HudBatch batch = hud.build(font, kWhite);
        CHECK(totalVertices(batch) == 6);  // the panel is unaffected
    }

    return iron_test_result();
}
