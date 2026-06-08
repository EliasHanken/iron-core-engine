#include "ui/UiStack.h"

#include "ui/UiLayout.h"
#include "ui/UiRender.h"

#include <utility>

namespace iron {

std::size_t UiStack::push(UiElement root, bool modal) {
    Screen s;
    s.root = std::move(root);
    s.modal = modal;
    uiAssignIds(s.root);
    screens_.push_back(std::move(s));
    return screens_.size() - 1;
}

void UiStack::pop() { if (!screens_.empty()) screens_.pop_back(); }
void UiStack::clear() { screens_.clear(); topHovered_ = 0; }

UiInputResult UiStack::updateDetailed(const UiInputState& in, Vec2 screenSize) {
    if (screens_.empty()) return {};
    Screen& s = screens_.back();
    UiLayoutMap m = layoutUi(s.root, screenSize);
    const UiClipMap clips = applyScroll(s.root, m, scrollOffsets_);
    UiInputResult r = updateUi(s.root, m, in, s.focused, drag_, clips);
    s.focused = r.focused;
    topHovered_ = r.hovered;
    drag_ = r.drag;
    lastMouse_ = in.mouse;
    for (const auto& d : r.scrollDeltas) scrollOffsets_[d.first] += d.second;
    return r;
}

std::vector<std::uint32_t> UiStack::update(const UiInputState& in, Vec2 screenSize) {
    return updateDetailed(in, screenSize).fired;
}

HudBatch UiStack::render(const FontAtlas& atlas, TextureHandle whiteTexture,
                         Vec2 screenSize) const {
    HudBatch out;
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        const Screen& s = screens_[i];
        const bool isTop = (i + 1 == screens_.size());
        UiLayoutMap m = layoutUi(s.root, screenSize);
        const UiClipMap clips = applyScroll(s.root, m, scrollOffsets_);
        const HudBatch b = renderUi(s.root, m, atlas, whiteTexture,
                                    isTop ? topHovered_ : 0,
                                    isTop ? s.focused : 0, clips);
        out.insert(out.end(), b.begin(), b.end());
    }
    // Drag ghost from the top screen, on top of everything.
    if (drag_.active && !screens_.empty()) {
        const Screen& s = screens_.back();
        UiLayoutMap m = layoutUi(s.root, screenSize);
        applyScroll(s.root, m, scrollOffsets_);
        const HudBatch g = renderUiDragGhost(s.root, m, atlas, whiteTexture, drag_, lastMouse_);
        out.insert(out.end(), g.begin(), g.end());
    }
    return out;
}

}  // namespace iron
