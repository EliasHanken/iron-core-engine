#pragma once

#include "math/Vec.h"
#include "render/HudBatch.h"
#include "render/Handles.h"
#include "ui/FontAtlas.h"
#include "ui/UiElement.h"
#include "ui/UiInput.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace iron {

// A stack of UI screens. Screens render bottom-to-top. Input routes to the TOP
// screen only — so a modal pause menu pushed over the HUD automatically blocks
// the HUD (and the game checks topIsModal() to freeze the world).
class UiStack {
public:
    std::size_t push(UiElement root, bool modal);   // (re)assigns ids; returns index
    void pop();
    void clear();
    bool empty() const { return screens_.empty(); }
    std::size_t size() const { return screens_.size(); }
    bool topIsModal() const { return !screens_.empty() && screens_.back().modal; }

    UiElement&       top()       { return screens_.back().root; }
    const UiElement& top() const { return screens_.back().root; }

    // Top screen's keyboard/gamepad focus. A game that rebuilds its screens each
    // frame (e.g. to refresh HUD values) must carry focus across the rebuild:
    // capture topFocus() after update(), re-seed it via setTopFocus() after the
    // rebuild. Widget ids are deterministic per tree structure, so a re-seeded id
    // still resolves to the same button.
    UiId topFocus() const { return screens_.empty() ? 0 : screens_.back().focused; }
    void setTopFocus(UiId id) { if (!screens_.empty()) screens_.back().focused = id; }

    // Carried drag state (persists across the per-frame rebuild like focus).
    UiDragState topDrag() const { return drag_; }
    void setTopDrag(const UiDragState& d) { drag_ = d; }

    // Per-scrollbox vertical scroll offset (px), accumulated from wheel input.
    float scrollOffset(UiId scrollBoxId) const {
        const auto it = scrollOffsets_.find(scrollBoxId);
        return it != scrollOffsets_.end() ? it->second : 0.0f;
    }
    void setScrollOffset(UiId scrollBoxId, float px) { scrollOffsets_[scrollBoxId] = px; }

    // Route input to the top screen; returns its fired actionIds.
    std::vector<std::uint32_t> update(const UiInputState& in, Vec2 screenSize);
    // Like update() but returns the full UiInputResult (drop, quickTransfer, etc.).
    UiInputResult updateDetailed(const UiInputState& in, Vec2 screenSize);
    // Render every screen bottom-to-top into one batch (with scroll clips + drag ghost).
    HudBatch render(const FontAtlas& atlas, TextureHandle whiteTexture, Vec2 screenSize) const;

private:
    struct Screen { UiElement root; bool modal = false; UiId focused = 0; };
    std::vector<Screen> screens_;
    UiId topHovered_ = 0;
    std::unordered_map<UiId, float> scrollOffsets_;
    UiDragState drag_;
    Vec2 lastMouse_{0.0f, 0.0f};
};

}  // namespace iron
