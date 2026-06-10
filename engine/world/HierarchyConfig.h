#pragma once

namespace iron {

// Depth cap shared by every parent-chain walk (scene-side and World-side);
// doubles as a cycle guard so a corrupted parent link never hangs.
inline constexpr int kMaxHierarchyDepth = 256;

}  // namespace iron
