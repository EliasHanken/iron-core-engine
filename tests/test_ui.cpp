#include "ui/UiElement.h"
#include "ui/UiLayout.h"
#include "test_framework.h"

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

    return iron_test_result();
}
