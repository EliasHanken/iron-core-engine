#pragma once

#include "math/Vec.h"
#include "net/NetworkStats.h"
#include "render/Handles.h"
#include "render/HudBatch.h"
#include "ui/BitmapFont.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// A handle to a retained HUD element. 0 is invalid; valid ids are 1-based.
using HudId = std::uint32_t;

// A retained-mode screen-space HUD. The game adds elements once, keeps their
// ids, and mutates them by id. build() turns the visible elements into a
// HudBatch of screen-space quads. Coordinates are pixels, origin top-left.
class Hud {
public:
    // Adds an element; returns its id. `color` is the text/panel colour or the
    // image tint.
    HudId addText(std::string text, Vec2 position, float scale, Vec4 color);
    HudId addPanel(Vec2 position, Vec2 size, Vec4 color);
    HudId addImage(Vec2 position, Vec2 size, TextureHandle texture, Vec4 tint);

    // Mutators. An out-of-range id is ignored.
    void setText(HudId id, std::string text);
    void setPosition(HudId id, Vec2 position);
    void setColor(HudId id, Vec4 color);
    void setSize(HudId id, Vec2 size);
    void setVisible(HudId id, bool visible);

    // ---------- network-stats convenience ----------
    // Bundles the HudIds for the 4 lines we render: connection state,
    // ping, packet loss, in/out bandwidth. Game creates one with
    // addNetworkStatsWidget and updates it each frame.
    struct NetStatsHudHandle {
        HudId pingId;
        HudId lossId;
        HudId bandwidthId;
        HudId stateId;
    };

    // Register 4 stacked text lines left-anchored just inside `topRight`.
    // Default text is "?"; call updateNetworkStats each frame.
    NetStatsHudHandle addNetworkStatsWidget(
        Vec2 topRight, Vec4 color = Vec4{1.0f, 1.0f, 1.0f, 0.7f});

    // Format a fresh ConnectionStats into the widget's 4 lines.
    void updateNetworkStats(const NetStatsHudHandle& h,
                            const ConnectionStats& s);

    // Builds the screen-space quad batch for the current visible elements.
    // Text quads use `font.atlas`; panels use `whiteTexture`; images use their
    // own texture. Quads are grouped by texture.
    HudBatch build(const BitmapFont& font, TextureHandle whiteTexture) const;

private:
    enum class Kind { Text, Panel, Image };

    struct Element {
        Kind kind = Kind::Panel;
        Vec2 position;
        Vec2 size;                              // Panel / Image
        Vec4 color;                             // colour or image tint
        std::string text;                       // Text
        float scale = 1.0f;                     // Text
        TextureHandle texture = kInvalidHandle;  // Image
        bool visible = true;
    };

    Element* get(HudId id);

    std::vector<Element> elements_;
};

} // namespace iron
