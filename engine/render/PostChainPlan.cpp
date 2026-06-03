#include "render/PostChainPlan.h"

namespace iron {

std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds) {
    // Copy always runs first as the opaque base (scene + bloom + SSAO + tonemap).
    // Effects are layered on top as blended overlays.
    std::vector<PostPass> passes{PostPass::Copy};

    bool xray = false, outline = false, glow = false;
    for (auto k : activeKinds) {
        if (k == EffectKind::XRay)             xray = true;
        else if (k == EffectKind::Outline)     outline = true;
        else if (k == EffectKind::GlowOutline) glow = true;
    }
    if (xray)    passes.push_back(PostPass::XRay);
    if (outline) passes.push_back(PostPass::Outline);
    if (glow) {
        passes.push_back(PostPass::GlowBlurH);
        passes.push_back(PostPass::GlowBlurV);
        passes.push_back(PostPass::GlowComposite);
    }
    return passes;
}

}  // namespace iron
