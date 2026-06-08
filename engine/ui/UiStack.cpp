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

std::vector<std::uint32_t> UiStack::update(const UiInputState& in, Vec2 screenSize) {
    if (screens_.empty()) return {};
    Screen& s = screens_.back();
    const UiLayoutMap m = layoutUi(s.root, screenSize);
    const UiInputResult r = updateUi(s.root, m, in, s.focused);
    s.focused = r.focused;
    topHovered_ = r.hovered;
    return r.fired;
}

HudBatch UiStack::render(const FontAtlas& atlas, TextureHandle whiteTexture,
                         Vec2 screenSize) const {
    HudBatch out;
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        const Screen& s = screens_[i];
        const bool isTop = (i + 1 == screens_.size());
        const UiLayoutMap m = layoutUi(s.root, screenSize);
        const HudBatch b = renderUi(s.root, m, atlas, whiteTexture,
                                    isTop ? topHovered_ : 0,
                                    isTop ? s.focused : 0);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

}  // namespace iron
