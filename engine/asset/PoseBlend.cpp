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

}  // namespace iron
