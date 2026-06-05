#include "asset/PoseBlend.h"

#include <algorithm>
#include <cmath>
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

namespace {
float wrap01(float time, float dur) {
    if (dur <= 0.0f) return 0.0f;
    float p = std::fmod(time, dur) / dur;
    if (p < 0.0f) p += 1.0f;
    return p;
}
void samplePhase(const Skeleton& sk, const AnimationClip& clip, float phase,
                 Pose& out) {
    const float t = (clip.duration > 0.0f) ? phase * clip.duration : 0.0f;
    samplePose(sk, clip, t, out);
}
}  // namespace

float blendSpaceDuration(const BlendSpace1D& space, float param) {
    if (space.samples.empty()) return 0.0f;
    if (space.samples.size() == 1 || param <= space.samples.front().first) {
        return space.samples.front().second
                   ? space.samples.front().second->duration : 0.0f;
    }
    if (param >= space.samples.back().first) {
        return space.samples.back().second
                   ? space.samples.back().second->duration : 0.0f;
    }
    std::size_t hi = 1;
    while (hi < space.samples.size() && space.samples[hi].first <= param) ++hi;
    const std::size_t lo = hi - 1;
    const float p0 = space.samples[lo].first;
    const float p1 = space.samples[hi].first;
    const float w = (p1 > p0) ? (param - p0) / (p1 - p0) : 0.0f;
    const float dLo = space.samples[lo].second
                          ? space.samples[lo].second->duration : 0.0f;
    const float dHi = space.samples[hi].second
                          ? space.samples[hi].second->duration : 0.0f;
    return dLo * (1.0f - w) + dHi * w;
}

void sampleBlendSpace(const Skeleton& skeleton, const BlendSpace1D& space,
                      float param, float time, Pose& out) {
    if (space.samples.empty()) { bindPose(skeleton, out); return; }

    if (space.samples.size() == 1 || param <= space.samples.front().first) {
        const AnimationClip* c = space.samples.front().second;
        if (!c) { bindPose(skeleton, out); return; }
        samplePhase(skeleton, *c, wrap01(time, c->duration), out);
        return;
    }
    if (param >= space.samples.back().first) {
        const AnimationClip* c = space.samples.back().second;
        if (!c) { bindPose(skeleton, out); return; }
        samplePhase(skeleton, *c, wrap01(time, c->duration), out);
        return;
    }
    std::size_t hi = 1;
    while (hi < space.samples.size() && space.samples[hi].first <= param) ++hi;
    const std::size_t lo = hi - 1;
    const float p0 = space.samples[lo].first;
    const float p1 = space.samples[hi].first;
    const float w = (p1 > p0) ? (param - p0) / (p1 - p0) : 0.0f;

    const AnimationClip* cLo = space.samples[lo].second;
    const AnimationClip* cHi = space.samples[hi].second;
    if (!cLo || !cHi) { bindPose(skeleton, out); return; }

    const float blendedDur = cLo->duration * (1.0f - w) + cHi->duration * w;
    const float phase = wrap01(time, blendedDur);
    Pose a, b;
    samplePhase(skeleton, *cLo, phase, a);
    samplePhase(skeleton, *cHi, phase, b);
    blendPose(a, b, w, out);
}

}  // namespace iron
