#include "ui/UiElement.h"
#include "ui/UiInput.h"
#include "ui/UiLayout.h"
#include "ui/FontAtlas.h"
#include "ui/UiRender.h"
#include "ui/UiStack.h"
#include "ui/UiSerialize.h"
#include "test_framework.h"
#include <algorithm>
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
            std::string(IRON_REPO_ROOT) + "/games/12-ui-arena/assets/fonts/Roboto-Medium.ttf";
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

    // UiStack: focus can be carried across a rebuild via setTopFocus/topFocus,
    // so a game that clears+rebuilds its screens each frame keeps keyboard focus.
    {
        auto buildMenu = [] {
            UiElement m = uiStackPanel(Anchor::Center, Vec2{0, 0}, Vec2{200, 120},
                                       StackDir::Vertical, 0.0f);
            m.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{200, 40},
                                          "Play", 18.0f, 1, Vec4{1, 1, 1, 1}));
            m.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{200, 40},
                                          "Quit", 18.0f, 2, Vec4{1, 1, 1, 1}));
            return m;
        };

        UiStack stack;
        stack.push(buildMenu(), false);
        const UiId quitId = stack.top().children[1].id;   // deterministic id

        // Seed focus on "Quit", then activate (no mouse, no nav this frame).
        stack.setTopFocus(quitId);
        UiInputState act; act.activate = true;
        std::vector<std::uint32_t> fired = stack.update(act, Vec2{800, 600});
        CHECK(fired.size() == 1u);
        CHECK(fired[0] == 2u);                 // focused "Quit" fired
        CHECK(stack.topFocus() == quitId);     // focus persisted, readable

        // Simulate the demo's per-frame rebuild: clear + rebuild resets focus to 0,
        // but re-seeding the carried id restores it (ids are stable per structure).
        const UiId carried = stack.topFocus();
        stack.clear();
        stack.push(buildMenu(), false);
        CHECK(stack.topFocus() == 0u);         // rebuild dropped focus...
        stack.setTopFocus(carried);            // ...re-seeded
        fired = stack.update(act, Vec2{800, 600});
        CHECK(fired.size() == 1u);
        CHECK(fired[0] == 2u);                 // still on "Quit" after the rebuild
    }

    // Serialize: a built tree round-trips through JSON (uiEqual ignores id/texture).
    {
        UiElement root = uiStackPanel(Anchor::Center, Vec2{0, 0}, Vec2{200, 160},
                                      StackDir::Vertical, 8.0f);
        root.children.push_back(uiButton(Anchor::TopCenter, Vec2{0, 0}, Vec2{180, 40},
                                         "Play", 20.0f, 1, Vec4{0.2f, 0.2f, 0.25f, 1}));
        root.children.push_back(uiBar(Anchor::BottomLeft, Vec2{10, -10}, Vec2{120, 14},
                                      0.7f, Vec4{0.8f, 0.2f, 0.2f, 1}, Vec4{0.1f, 0.1f, 0.1f, 1}));
        uiAssignIds(root);

        const UiElement back = uiFromJson(uiToJson(root));
        CHECK(uiEqual(root, back));
    }

    // M63: new kinds, slot flags, and serialize round-trip for new fields.
    {
        UiElement grid = uiGrid(Anchor::TopLeft, Vec2{10, 10}, Vec2{200, 120}, 4, 6.0f);
        CHECK(grid.kind == UiKind::Grid);
        CHECK(grid.gridCols == 4);
        CHECK(grid.spacing == 6.0f);

        UiElement scroll = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{180, 100}, 4.0f);
        CHECK(scroll.kind == UiKind::ScrollBox);

        UiElement slot = uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                kInvalidHandle, Vec4{8, 8, 8, 8}, 0.25f,
                                /*userData=*/0x00010003u);
        CHECK(slot.kind == UiKind::Image);
        CHECK(slot.draggable);
        CHECK(slot.dropTarget);
        CHECK(slot.userData == 0x00010003u);
        CHECK(slot.nineSliceUv == 0.25f);
        CHECK(slot.nineSliceMargin.x == 8.0f);

        // Serialize round-trips the new fields (texture/id still excluded).
        const nlohmann::json j = uiToJson(slot);
        const UiElement back = uiFromJson(j);
        CHECK(uiEqual(slot, back));
        // uiEqual is sensitive to the new fields.
        UiElement diff = slot; diff.userData = 99u;
        CHECK(!uiEqual(slot, diff));
        UiElement diff2 = slot; diff2.gridCols = 7;
        CHECK(!uiEqual(slot, diff2));
    }

    // M63: Grid wraps children into rows of `gridCols` using child size + spacing.
    {
        UiElement grid = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200}, 3, 10.0f);
        for (int i = 0; i < 5; ++i)
            grid.children.push_back(uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                            Vec4{1, 1, 1, 1}));
        uiAssignIds(grid);
        const UiLayoutMap m = layoutUi(grid, Vec2{300, 300});
        // child 0 at (0,0); child 1 at (50,0); child 2 at (100,0); child 3 wraps to (0,50).
        CHECK(m.at(grid.children[0].id).min.x == 0.0f);
        CHECK(m.at(grid.children[1].id).min.x == 50.0f);
        CHECK(m.at(grid.children[2].id).min.x == 100.0f);
        CHECK(m.at(grid.children[3].id).min.x == 0.0f);
        CHECK(m.at(grid.children[3].id).min.y == 50.0f);
    }

    // M63: applyScroll shifts ScrollBox descendants up by the offset and clips them.
    {
        UiElement box = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, 0.0f);
        UiElement inner = uiGrid(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 300}, 1, 0.0f);
        for (int i = 0; i < 6; ++i)                          // 6 * 50 = 300 tall content
            inner.children.push_back(uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 50},
                                             Vec4{1, 1, 1, 1}));
        box.children.push_back(std::move(inner));
        uiAssignIds(box);
        UiLayoutMap m = layoutUi(box, Vec2{200, 400});
        const UiId child0 = box.children[0].children[0].id;
        const float before = m.at(child0).min.y;
        std::unordered_map<UiId, float> offsets{{box.id, 40.0f}};
        const UiClipMap clips = applyScroll(box, m, offsets);
        CHECK(m.at(child0).min.y == before - 40.0f);         // shifted up
        CHECK(clips.count(child0) == 1u);                    // descendant is clipped
        CHECK(clips.at(child0).min.y == 0.0f);               // clip == scrollbox rect
        CHECK(clips.at(child0).max.y == 100.0f);
        // Offset clamps to [0, contentHeight - viewport] = [0, 200].
        std::unordered_map<UiId, float> tooBig{{box.id, 9999.0f}};
        UiLayoutMap m2 = layoutUi(box, Vec2{200, 400});
        applyScroll(box, m2, tooBig);
        const UiId last = box.children[0].children[5].id;
        // content bottom (originally 300) shifted by clamped 200 -> 100 == viewport bottom.
        CHECK(m2.at(last).max.y == 100.0f);
    }

    // M63: uiClipQuad intersects a quad and remaps UVs proportionally.
    {
        Vec2 mn{0, 0}, mx{100, 100}, uv0{0, 0}, uv1{1, 1};
        const Rect clip{Vec2{50, 0}, Vec2{200, 200}};       // clip left half away
        const bool kept = uiClipQuad(mn, mx, uv0, uv1, clip);
        CHECK(kept);
        CHECK(mn.x == 50.0f);
        CHECK(uv0.x == 0.5f);                                // uv remapped to half
        CHECK(mx.x == 100.0f);
        // Fully outside -> dropped.
        Vec2 a{0, 0}, b{10, 10}, c{0, 0}, d{1, 1};
        CHECK(!uiClipQuad(a, b, c, d, Rect{Vec2{500, 500}, Vec2{600, 600}}));
    }

    // M63: a 9-slice image emits 9 quads (54 verts) into its texture group.
    {
        FontAtlas dummy;                                     // no texture; not used here
        UiElement panel = uiImage9(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100},
                                   /*tex=*/7, Vec4{10, 10, 10, 10}, 0.25f, Vec4{1, 1, 1, 1});
        uiAssignIds(panel);
        const UiLayoutMap m = layoutUi(panel, Vec2{200, 200});
        const HudBatch b = renderUi(panel, m, dummy, /*white=*/1, 0, 0);
        std::size_t verts = 0;
        for (const auto& g : b) if (g.texture == 7) verts += g.vertices.size();
        CHECK(verts == 54u);                                 // 9 quads * 6 verts
    }

    // M63: clips param trims a panel's quad to the clip rect.
    {
        FontAtlas dummy;
        UiElement p = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, Vec4{1, 1, 1, 1});
        uiAssignIds(p);
        const UiLayoutMap m = layoutUi(p, Vec2{200, 200});
        UiClipMap clips; clips[p.id] = Rect{Vec2{0, 0}, Vec2{100, 40}};
        const HudBatch b = renderUi(p, m, dummy, /*white=*/1, 0, 0, clips);
        // The single quad's lowest y must be clamped to 40.
        float maxY = 0.0f;
        for (const auto& g : b) for (const auto& v : g.vertices) maxY = std::max(maxY, v.position.y);
        CHECK(maxY == 40.0f);
    }

    // M63: drag ghost re-renders the dragged subtree translated to the cursor.
    {
        FontAtlas dummy;
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{200, 200}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40},
                                       /*tile=*/0, Vec4{0, 0, 0, 0}, 0.0f, 0x10001u));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{200, 200});
        UiDragState drag; drag.active = true; drag.sourceId = root.children[0].id;
        drag.grabOffset = Vec2{0, 0};
        const HudBatch ghost = renderUiDragGhost(root, m, dummy, /*white=*/1, drag, Vec2{120, 130});
        bool any = false;
        for (const auto& g : ghost) if (!g.vertices.empty()) any = true;
        CHECK(any);                                          // ghost emitted something
        // Ghost geometry sits near the cursor (x >= ~120), not at the origin slot.
        float minX = 1e9f;
        for (const auto& g : ghost) for (const auto& v : g.vertices) minX = std::min(minX, v.position.x);
        CHECK(minX >= 120.0f - 0.01f);
    }

    // M63: drag start on press over a draggable; release over a dropTarget emits a drop.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{300, 100}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0},   Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00000001u));  // src slot
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{100, 0}, Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00010002u));  // dst slot
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{300, 100});

        // Frame 1: press on src slot -> drag becomes active.
        UiInputState s1; s1.mouse = Vec2{20, 20}; s1.mousePressed = true; s1.mouseDown = true;
        UiInputResult r1 = updateUi(root, m, s1, 0, UiDragState{}, UiClipMap{});
        CHECK(r1.drag.active);
        CHECK(r1.drag.sourceUserData == 0x00000001u);

        // Frame 2: release over dst slot -> drop{src,dst}, drag clears.
        UiInputState s2; s2.mouse = Vec2{120, 20}; s2.mouseReleased = true;
        UiInputResult r2 = updateUi(root, m, s2, 0, r1.drag, UiClipMap{});
        CHECK(r2.drop.has_value());
        CHECK(r2.drop->source == 0x00000001u);
        CHECK(r2.drop->target == 0x00010002u);
        CHECK(!r2.drag.active);
    }

    // M63: double-click on a draggable emits quickTransfer, no drag.
    {
        UiElement root = uiPanel(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, Vec4{0, 0, 0, 0});
        root.children.push_back(uiSlot(Anchor::TopLeft, Vec2{0, 0}, Vec2{40, 40}, 0,
                                       Vec4{0, 0, 0, 0}, 0.0f, 0x00000005u));
        uiAssignIds(root);
        const UiLayoutMap m = layoutUi(root, Vec2{100, 100});
        UiInputState s; s.mouse = Vec2{20, 20}; s.mousePressed = true; s.mouseDown = true;
        s.doubleClick = true;
        UiInputResult r = updateUi(root, m, s, 0, UiDragState{}, UiClipMap{});
        CHECK(r.quickTransfer.has_value());
        CHECK(*r.quickTransfer == 0x00000005u);
        CHECK(!r.drag.active);
    }

    // M63: wheel over a scrollbox produces a scroll delta keyed by its id.
    {
        UiElement box = uiScrollBox(Anchor::TopLeft, Vec2{0, 0}, Vec2{100, 100}, 0.0f);
        uiAssignIds(box);
        const UiLayoutMap m = layoutUi(box, Vec2{200, 200});
        UiInputState s; s.mouse = Vec2{50, 50}; s.wheel = -1.0f;   // scroll down
        UiInputResult r = updateUi(box, m, s, 0, UiDragState{}, UiClipMap{});
        CHECK(r.scrollDeltas.size() == 1u);
        CHECK(r.scrollDeltas[0].first == box.id);
        CHECK(r.scrollDeltas[0].second > 0.0f);               // down -> increase offset
    }

    return iron_test_result();
}
