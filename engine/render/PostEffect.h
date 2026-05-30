#pragma once

#include "math/Vec.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iron {

enum class EffectKind : uint8_t { None = 0, Outline, GlowOutline, XRay };

struct EffectStyle {
    EffectKind kind      = EffectKind::None;
    Vec3       color     = {1.0f, 0.6f, 0.1f};  // selection orange
    float      width     = 2.0f;                 // outline thickness / blur radius (px)
    float      intensity = 1.0f;                 // glow strength / x-ray opacity
};

// Fixed 256-entry table: effect id -> style. Id 0 reserved as "no effect" and
// always reports None. The renderer indexes this by DrawCall::effectId.
class EffectTable {
public:
    static constexpr int kMaxIds = 256;

    void setStyle(uint8_t id, const EffectStyle& s) {
        if (id == 0) return;                 // id 0 is always None
        styles_[id] = s;
    }
    const EffectStyle& style(uint8_t id) const { return styles_[id]; }

    std::vector<EffectKind> activeKinds() const {
        std::array<bool, 4> seen{};
        for (int i = 1; i < kMaxIds; ++i)
            seen[static_cast<int>(styles_[i].kind)] = true;
        std::vector<EffectKind> out;
        for (int k = 1; k < 4; ++k)          // skip None(0)
            if (seen[k]) out.push_back(static_cast<EffectKind>(k));
        return out;
    }

private:
    std::array<EffectStyle, kMaxIds> styles_{};
};

}  // namespace iron
