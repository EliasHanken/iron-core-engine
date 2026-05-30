#include "render/PostChainPlan.h"

namespace iron {

std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds) {
    if (activeKinds.empty()) return {PostPass::Copy};

    std::vector<PostPass> passes;
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
