#pragma once

#include "math/Vec.h"
#include "ui/UiElement.h"
#include "ui/UiLayout.h"

#include <cstdint>
#include <vector>

namespace iron {

// One frame of UI input. Edge flags (`*Pressed`) are true only on the frame the
// key/button transitions down — the game fills these from iron::Input.
struct UiInputState {
    Vec2 mouse{0.0f, 0.0f};
    bool mousePressed = false;   // left mouse went down this frame
    bool navPrev = false;        // up/left went down this frame
    bool navNext = false;        // down/right went down this frame
    bool activate = false;       // enter/space/gamepad-A went down this frame
};

struct UiInputResult {
    std::vector<std::uint32_t> fired;  // actionIds activated this frame
    UiId hovered = 0;                  // button under the cursor (0 = none)
    UiId focused = 0;                  // keyboard/gamepad focus (0 = none)
};

// Process one screen's tree. `prevFocused` carries focus across frames. Mouse:
// the topmost visible Button under the cursor is `hovered`; a click on it fires
// its actionId and takes focus. Keyboard/gamepad: navNext/navPrev move focus
// among visible Buttons in tree order (wrapping); activate fires the focused
// button. A button fires at most once per frame.
UiInputResult updateUi(const UiElement& root, const UiLayoutMap& rects,
                       const UiInputState& in, UiId prevFocused);

}  // namespace iron
