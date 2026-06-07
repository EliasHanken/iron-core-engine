#include "ui/UiElement.h"
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

    return iron_test_result();
}
