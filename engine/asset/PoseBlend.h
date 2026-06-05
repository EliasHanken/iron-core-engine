#pragma once

#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/Skeleton.h"

#include <utility>
#include <vector>

namespace iron {

// Per-bone blend: lerp translation/scale, slerp rotation, by weight t in
// [0,1] (t=0 -> a, t=1 -> b; t is clamped). Operates over min(a,b) bones;
// `out` is resized to that count.
void blendPose(const Pose& a, const Pose& b, float t, Pose& out);

}  // namespace iron
