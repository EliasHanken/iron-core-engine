#pragma once

#include "render/PostEffect.h"

#include <vector>

namespace iron {

enum class PostPass : uint8_t {
    Copy, Outline, GlowBlurH, GlowBlurV, GlowComposite, XRay,
};

// Given the distinct active effect kinds this frame, produce the ordered passes.
// Empty -> {Copy}. Layer order: XRay (under), Outline, Glow (over).
std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds);

inline int pingPongSource(int passIndex) { return passIndex % 2; }
inline int pingPongDest(int passIndex)   { return (passIndex + 1) % 2; }

}  // namespace iron
