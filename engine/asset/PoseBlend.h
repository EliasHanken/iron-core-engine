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

// A 1D blend space: clips placed along a scalar axis (e.g. speed). Samples are
// kept sorted ascending by param via add().
struct BlendSpace1D {
    std::vector<std::pair<float, const AnimationClip*>> samples;

    // Insert (param, clip) keeping `samples` sorted ascending by param.
    void add(float param, const AnimationClip* clip);
};

// Sample the blend space at `param` and `time`: find the two bracketing
// samples, sample both clips at `time`, and blendPose by the normalized
// weight. Clamps to the end clips outside the param range. With a single
// sample, returns that clip's pose. With no samples, returns the bind pose.
void sampleBlendSpace(const Skeleton& skeleton, const BlendSpace1D& space,
                      float param, float time, Pose& out);

// The effective (blended) duration of the two bracketing clips at `param`,
// used to drive a normalized playback phase. Clamps to the ends; single
// sample -> that clip's duration; empty/null -> 0.
float blendSpaceDuration(const BlendSpace1D& space, float param);

}  // namespace iron
