#include "asset/PoseBlend.h"

#include <algorithm>
#include <cstddef>

namespace iron {

void blendPose(const Pose& a, const Pose& b, float t, Pose& out) {
    t = std::clamp(t, 0.0f, 1.0f);
    const std::size_t n = std::min(a.bones.size(), b.bones.size());
    out.bones.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.bones[i].translation = interpolate(a.bones[i].translation,
                                               b.bones[i].translation, t);
        out.bones[i].scale = interpolate(a.bones[i].scale, b.bones[i].scale, t);
        out.bones[i].rotation = slerp(a.bones[i].rotation, b.bones[i].rotation, t);
    }
}

void BlendSpace1D::add(float param, const AnimationClip* clip) {
    auto it = samples.begin();
    while (it != samples.end() && it->first < param) ++it;
    samples.insert(it, std::make_pair(param, clip));
}

void sampleBlendSpace(const Skeleton& skeleton, const BlendSpace1D& space,
                      float param, float time, Pose& out) {
    if (space.samples.empty()) {
        bindPose(skeleton, out);
        return;
    }
    if (space.samples.size() == 1 || param <= space.samples.front().first) {
        if (!space.samples.front().second) {
            bindPose(skeleton, out);
            return;
        }
        samplePose(skeleton, *space.samples.front().second, time, out);
        return;
    }
    if (param >= space.samples.back().first) {
        if (!space.samples.back().second) {
            bindPose(skeleton, out);
            return;
        }
        samplePose(skeleton, *space.samples.back().second, time, out);
        return;
    }
    // Find the bracket [lo, hi] with samples[lo].param <= param < samples[hi].
    std::size_t hi = 1;
    while (hi < space.samples.size() && space.samples[hi].first <= param) ++hi;
    const std::size_t lo = hi - 1;
    const float p0 = space.samples[lo].first;
    const float p1 = space.samples[hi].first;
    const float w = (p1 > p0) ? (param - p0) / (p1 - p0) : 0.0f;

    if (!space.samples[lo].second || !space.samples[hi].second) {
        bindPose(skeleton, out);
        return;
    }
    Pose a, b;
    samplePose(skeleton, *space.samples[lo].second, time, a);
    samplePose(skeleton, *space.samples[hi].second, time, b);
    blendPose(a, b, w, out);
}

}  // namespace iron
