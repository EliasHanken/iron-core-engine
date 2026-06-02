#include "editor/ViewportInput.h"
#include "test_framework.h"

int main() {
    const iron::Vec2 rectMin{100.0f, 50.0f};
    const iron::Vec2 rectSize{800.0f, 600.0f};

    // Inside: center of the rect maps to local (400,300).
    {
        iron::Vec2 local{};
        const bool inside = iron::viewportLocalMouse({500.0f, 350.0f}, rectMin, rectSize, local);
        CHECK(inside);
        CHECK_NEAR(local.x, 400.0f);
        CHECK_NEAR(local.y, 300.0f);
    }
    // Top-left corner → local (0,0).
    {
        iron::Vec2 local{};
        CHECK(iron::viewportLocalMouse({100.0f, 50.0f}, rectMin, rectSize, local));
        CHECK_NEAR(local.x, 0.0f);
        CHECK_NEAR(local.y, 0.0f);
    }
    // Outside left → false.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({50.0f, 350.0f}, rectMin, rectSize, local));
    }
    // Outside below → false.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({500.0f, 700.0f}, rectMin, rectSize, local));
    }
    // Zero-size rect → never inside.
    {
        iron::Vec2 local{};
        CHECK(!iron::viewportLocalMouse({100.0f, 50.0f}, rectMin, {0.0f, 0.0f}, local));
    }
    return iron_test_result();
}
