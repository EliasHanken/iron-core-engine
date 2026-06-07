#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiLayout.h"
#include "ui/FontAtlas.h"
#include "ui/UiRender.h"
#include "ui/UiStack.h"
#include "test_framework.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace iron;

int main() {
    // Builders set kind + fields; a stack panel groups children.
    {
        UiElement menu = uiStackPanel(Anchor::Center, Vec2{0, 0}, Vec2{200, 160},
                                      StackDir::Vertical, 8.0f);
        menu.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Play", 20.0f, 1, Vec4{0.2f, 0.2f, 0.25f, 1}));
        menu.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Quit", 20.0f, 2, Vec4{0.2f, 0.2f, 0.25f, 1}));

        CHECK(menu.kind == UiKind::Panel);
        CHECK(menu.stack == StackDir::Vertical);
        CHECK(menu.children.size() == 2u);
        CHECK(menu.children[0].kind == UiKind::Button);
        CHECK(menu.children[0].actionId == 1u);
        CHECK(menu.children[1].text == "Quit");

        // Ids are unique + sequential, depth-first.
        const UiId next = uiAssignIds(menu);
        CHECK(menu.id == 1u);
        CHECK(menu.children[0].id == 2u);
        CHECK(menu.children[1].id == 3u);
        CHECK(next == 4u);
    }

    // Layout: anchors resolve against the parent; Stretch insets by offset.
    {
        UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0},
                                 Vec4{0, 0, 0, 0});
        // top-left 100x40 at (10,10)
        root.children.push_back(uiPanel(Anchor::TopLeft, Vec2{10, 10}, Vec2{100, 40},
                                        Vec4{1, 1, 1, 1}));
        // bottom-right 50x50 inset (20,20) from the corner
        root.children.push_back(uiPanel(Anchor::BottomRight, Vec2{-20, -20}, Vec2{50, 50},
                                        Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        const Rect rr = m.at(root.id);
        CHECK_NEAR(rr.min.x, 0.0f); CHECK_NEAR(rr.max.x, 800.0f);

        const Rect tl = m.at(root.children[0].id);
        CHECK_NEAR(tl.min.x, 10.0f); CHECK_NEAR(tl.min.y, 10.0f);
        CHECK_NEAR(tl.max.x, 110.0f); CHECK_NEAR(tl.max.y, 50.0f);

        const Rect br = m.at(root.children[1].id);
        CHECK_NEAR(br.max.x, 800.0f - 20.0f);  // right edge minus offset
        CHECK_NEAR(br.max.y, 600.0f - 20.0f);
        CHECK_NEAR(br.min.x, 800.0f - 20.0f - 50.0f);
    }

    // Layout: a vertical stack places children top-down with spacing.
    {
        UiElement stack = uiStackPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 300},
                                       StackDir::Vertical, 10.0f);
        stack.children.push_back(uiPanel(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         Vec4{1, 1, 1, 1}));
        stack.children.push_back(uiPanel(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         Vec4{1, 1, 1, 1}));
        uiAssignIds(stack);
        const UiLayoutMap m = layoutUi(stack, Vec2{800, 600});

        const Rect a = m.at(stack.children[0].id);
        const Rect b = m.at(stack.children[1].id);
        CHECK_NEAR(a.min.y, 0.0f);
        CHECK_NEAR(b.min.y, 50.0f);           // 40 height + 10 spacing
        CHECK_NEAR(a.min.x, 10.0f);           // centered: (200-180)/2
        CHECK_NEAR(b.min.x, 10.0f);
    }

    // Input: a click inside a button fires its actionId; outside does not.
    {
        UiElement root = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                         "Play", 18.0f, 7, Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        UiInputState in;
        in.mouse = Vec2{50, 25}; in.mousePressed = true;
        UiInputResult r = updateUi(root, m, in, 0);
        CHECK(r.hovered == root.children[0].id);
        CHECK(r.fired.size() == 1u);
        CHECK(r.fired[0] == 7u);

        in.mouse = Vec2{500, 500};   // outside the button
        r = updateUi(root, m, in, 0);
        CHECK(r.hovered == 0u);
        CHECK(r.fired.empty());
    }

    // Input: nav cycles focus among buttons (wrapping); activate fires focused.
    {
        UiElement root = uiStackPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200},
                                      StackDir::Vertical, 0.0f);
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 40},
                                         "A", 18.0f, 11, Vec4{1, 1, 1, 1}));
        root.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 40},
                                         "B", 18.0f, 22, Vec4{1, 1, 1, 1}));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});
        const UiId bA = root.children[0].id;
        const UiId bB = root.children[1].id;

        UiInputState nav; nav.navNext = true;
        UiInputResult r = updateUi(root, m, nav, 0);
        CHECK(r.focused == bA);                 // first navNext focuses first button
        r = updateUi(root, m, nav, r.focused);
        CHECK(r.focused == bB);                 // advances
        r = updateUi(root, m, nav, r.focused);
        CHECK(r.focused == bA);                 // wraps

        UiInputState act; act.activate = true;
        r = updateUi(root, m, act, bB);
        CHECK(r.focused == bB);
        CHECK(r.fired.size() == 1u);
        CHECK(r.fired[0] == 22u);
    }

    // FontAtlas: baking Roboto yields a non-empty atlas + sane metrics.
    {
        const std::string path =
            std::string(IRON_REPO_ROOT) + "/games/11-sandbox/assets/fonts/Roboto-Medium.ttf";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
            const std::size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            CHECK(rd == bytes.size());

            FontAtlas atlas;
            CHECK(atlas.bake(bytes.data(), static_cast<int>(bytes.size()), 48.0f));
            CHECK(atlas.width() > 0);
            CHECK(atlas.height() > 0);
            CHECK(atlas.pixels().size() ==
                  static_cast<std::size_t>(atlas.width()) * atlas.height() * 4);
            CHECK(atlas.textWidth("AV") > 0.0f);
            CHECK(atlas.textWidth("AVA") > atlas.textWidth("AV"));  // proportional advance

            float penX = 0.0f, penY = 0.0f;
            atlas.quadFor('A', penX, penY);
            CHECK(penX > 0.0f);   // pen advanced
        }
    }

    // Render: a panel emits one white-texture quad (6 verts) at its laid-out rect.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 40},
                                 Vec4{0.5f, 0.5f, 0.5f, 1.0f});
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{800, 600});

        FontAtlas empty;                 // not baked: text routines no-op
        const TextureHandle white = static_cast<TextureHandle>(1);
        const HudBatch b = renderUi(root, m, empty, white, 0, 0);

        CHECK(b.size() == 1u);
        CHECK(b[0].texture == white);
        CHECK(b[0].vertices.size() == 6u);
        CHECK_NEAR(b[0].vertices[0].position.x, 0.0f);
        CHECK_NEAR(b[0].vertices[0].position.y, 0.0f);
    }

    // Render: a Bar emits the track quad plus a partial fill quad (2 quads).
    {
        UiElement bar = uiBar(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 20}, 0.5f,
                              Vec4{1, 0, 0, 1}, Vec4{0.2f, 0.2f, 0.2f, 1});
        uiAssignIds(bar);
        const UiLayoutMap m = layoutUi(bar, Vec2{800, 600});
        FontAtlas empty;
        const TextureHandle white = static_cast<TextureHandle>(1);
        const HudBatch b = renderUi(bar, m, empty, white, 0, 0);

        CHECK(b.size() == 1u);                  // both quads use whiteTexture
        CHECK(b[0].vertices.size() == 12u);     // track + fill = 2 quads
    }

    // UiStack: a modal top screen blocks input to the screen beneath it.
    {
        UiStack stack;
        UiElement hud = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0});
        hud.children.push_back(uiButton(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                        "HudBtn", 18.0f, 99, Vec4{1, 1, 1, 1}));
        stack.push(hud, /*modal=*/false);

        UiElement pause = uiPanel(Anchor::Stretch, Vec2{0, 0}, Vec2{0, 0}, Vec4{0, 0, 0, 0.5f});
        pause.children.push_back(uiButton(Anchor::Center, Vec2{0, 0}, Vec2{120, 40},
                                          "Resume", 18.0f, 3, Vec4{1, 1, 1, 1}));
        stack.push(pause, /*modal=*/true);
        CHECK(stack.topIsModal());

        // Click where the HUD button sits (top-left): it must NOT fire — only the
        // top (pause) screen receives input, and nothing of pause is there.
        UiInputState in; in.mouse = Vec2{50, 25}; in.mousePressed = true;
        std::vector<std::uint32_t> fired = stack.update(in, Vec2{800, 600});
        CHECK(fired.empty());

        // Pop the pause screen; now the HUD button is clickable again.
        stack.pop();
        CHECK(!stack.topIsModal());
        fired = stack.update(in, Vec2{800, 600});
        CHECK(fired.size() == 1u);
        CHECK(fired[0] == 99u);
    }

    return iron_test_result();
}
