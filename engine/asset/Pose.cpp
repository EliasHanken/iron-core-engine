#include "asset/Pose.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace iron {

namespace {

// Returns index i with inputs[i] <= t < inputs[i+1], clamped to the ends;
// (size_t)-1 for an empty inputs vector (callers handle).
std::size_t findKeyframe(const std::vector<float>& inputs, float t) {
    if (inputs.empty()) return static_cast<std::size_t>(-1);
    if (t <= inputs.front()) return 0;
    if (t >= inputs.back()) return inputs.size() - 1;
    auto it = std::upper_bound(inputs.begin(), inputs.end(), t);
    return static_cast<std::size_t>((it - inputs.begin()) - 1);
}

Vec3 sampleVec3(const AnimationSampler& s, float t) {
    const std::size_t i = findKeyframe(s.inputs, t);
    if (i == static_cast<std::size_t>(-1)) return Vec3{0, 0, 0};
    const Vec3 v0{s.outputs[i * 3 + 0], s.outputs[i * 3 + 1], s.outputs[i * 3 + 2]};
    if (i + 1 >= s.inputs.size() || s.interpolation == AnimationInterpolation::Step) {
        return v0;
    }
    const Vec3 v1{s.outputs[(i + 1) * 3 + 0], s.outputs[(i + 1) * 3 + 1],
                  s.outputs[(i + 1) * 3 + 2]};
    const float a = (t - s.inputs[i]) / (s.inputs[i + 1] - s.inputs[i]);
    return interpolate(v0, v1, a);
}

Quat sampleQuat(const AnimationSampler& s, float t) {
    const std::size_t i = findKeyframe(s.inputs, t);
    if (i == static_cast<std::size_t>(-1)) return Quat::identity();
    const Quat q0{s.outputs[i * 4 + 0], s.outputs[i * 4 + 1],
                  s.outputs[i * 4 + 2], s.outputs[i * 4 + 3]};
    if (i + 1 >= s.inputs.size() || s.interpolation == AnimationInterpolation::Step) {
        return q0;
    }
    const Quat q1{s.outputs[(i + 1) * 4 + 0], s.outputs[(i + 1) * 4 + 1],
                  s.outputs[(i + 1) * 4 + 2], s.outputs[(i + 1) * 4 + 3]};
    const float a = (t - s.inputs[i]) / (s.inputs[i + 1] - s.inputs[i]);
    return Quat::slerp(q0, q1, a);
}

}  // namespace

Mat4 composeTRS(Vec3 t, Quat r, Vec3 s) {
    Mat4 m = r.toMat4();
    m.at(0, 0) *= s.x; m.at(1, 0) *= s.x; m.at(2, 0) *= s.x;
    m.at(0, 1) *= s.y; m.at(1, 1) *= s.y; m.at(2, 1) *= s.y;
    m.at(0, 2) *= s.z; m.at(1, 2) *= s.z; m.at(2, 2) *= s.z;
    m.at(0, 3) = t.x; m.at(1, 3) = t.y; m.at(2, 3) = t.z;
    m.at(3, 3) = 1.0f;
    return m;
}

Mat4 composeTRS(const BoneLocal& b) {
    return composeTRS(b.translation, b.rotation, b.scale);
}

BoneLocal decomposeTRS(const Mat4& m) {
    BoneLocal out;
    out.translation = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    const Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    out.scale = Vec3{length(c0), length(c1), length(c2)};

    Mat4 rot = Mat4::identity();
    if (out.scale.x > 1e-8f) { rot.at(0, 0) = c0.x / out.scale.x; rot.at(1, 0) = c0.y / out.scale.x; rot.at(2, 0) = c0.z / out.scale.x; }
    if (out.scale.y > 1e-8f) { rot.at(0, 1) = c1.x / out.scale.y; rot.at(1, 1) = c1.y / out.scale.y; rot.at(2, 1) = c1.z / out.scale.y; }
    if (out.scale.z > 1e-8f) { rot.at(0, 2) = c2.x / out.scale.z; rot.at(1, 2) = c2.y / out.scale.z; rot.at(2, 2) = c2.z / out.scale.z; }

    Quat& r = out.rotation;
    const float trace = rot.at(0, 0) + rot.at(1, 1) + rot.at(2, 2);
    if (trace > 0.0f) {
        const float sq = std::sqrt(trace + 1.0f) * 2.0f;
        r.w = 0.25f * sq;
        r.x = (rot.at(2, 1) - rot.at(1, 2)) / sq;
        r.y = (rot.at(0, 2) - rot.at(2, 0)) / sq;
        r.z = (rot.at(1, 0) - rot.at(0, 1)) / sq;
    } else if (rot.at(0, 0) > rot.at(1, 1) && rot.at(0, 0) > rot.at(2, 2)) {
        const float sq = std::sqrt(1.0f + rot.at(0, 0) - rot.at(1, 1) - rot.at(2, 2)) * 2.0f;
        r.w = (rot.at(2, 1) - rot.at(1, 2)) / sq;
        r.x = 0.25f * sq;
        r.y = (rot.at(0, 1) + rot.at(1, 0)) / sq;
        r.z = (rot.at(0, 2) + rot.at(2, 0)) / sq;
    } else if (rot.at(1, 1) > rot.at(2, 2)) {
        const float sq = std::sqrt(1.0f + rot.at(1, 1) - rot.at(0, 0) - rot.at(2, 2)) * 2.0f;
        r.w = (rot.at(0, 2) - rot.at(2, 0)) / sq;
        r.x = (rot.at(0, 1) + rot.at(1, 0)) / sq;
        r.y = 0.25f * sq;
        r.z = (rot.at(1, 2) + rot.at(2, 1)) / sq;
    } else {
        const float sq = std::sqrt(1.0f + rot.at(2, 2) - rot.at(0, 0) - rot.at(1, 1)) * 2.0f;
        r.w = (rot.at(1, 0) - rot.at(0, 1)) / sq;
        r.x = (rot.at(0, 2) + rot.at(2, 0)) / sq;
        r.y = (rot.at(1, 2) + rot.at(2, 1)) / sq;
        r.z = 0.25f * sq;
    }
    r = r.normalized();
    return out;
}

void bindPose(const Skeleton& skeleton, Pose& out) {
    out.bones.resize(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        out.bones[i] = decomposeTRS(skeleton.bones[i].localBindTransform);
    }
}

void samplePose(const Skeleton& skeleton, const AnimationClip& clip,
                float time, Pose& out) {
    const std::size_t n = skeleton.bones.size();
    bindPose(skeleton, out);  // seed every bone from bind pose
    for (const auto& ch : clip.channels) {
        if (ch.targetBone < 0 || static_cast<std::size_t>(ch.targetBone) >= n) continue;
        if (ch.samplerIndex < 0 ||
            static_cast<std::size_t>(ch.samplerIndex) >= clip.samplers.size()) continue;
        const AnimationSampler& s = clip.samplers[ch.samplerIndex];
        switch (ch.path) {
            case AnimationPath::Translation:
                out.bones[ch.targetBone].translation = sampleVec3(s, time);
                break;
            case AnimationPath::Rotation:
                out.bones[ch.targetBone].rotation = sampleQuat(s, time);
                break;
            case AnimationPath::Scale:
                out.bones[ch.targetBone].scale = sampleVec3(s, time);
                break;
        }
    }
}

void composeGlobals(const Skeleton& skeleton, const Pose& pose,
                    std::span<Mat4> outGlobals) {
    const std::size_t n = std::min({outGlobals.size(), pose.bones.size(),
                                    skeleton.bones.size()});
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4 local = composeTRS(pose.bones[i]);
        const int parent = skeleton.bones[i].parentIndex;
        if (parent < 0) {
            outGlobals[i] = local;
        } else {
            assert(static_cast<std::size_t>(parent) < i &&
                   "bone parent must have lower index");
            outGlobals[i] = outGlobals[parent] * local;
        }
    }
}

void globalsToPalette(const Skeleton& skeleton, std::span<const Mat4> globals,
                      std::span<Mat4> out) {
    const std::size_t n = std::min({globals.size(), out.size(),
                                    skeleton.bones.size()});
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = globals[i] * skeleton.bones[i].inverseBindMatrix;
    }
}

void posePalette(const Skeleton& skeleton, const Pose& pose,
                 std::span<Mat4> out) {
    const std::size_t n = std::min({out.size(), pose.bones.size(),
                                    skeleton.bones.size()});
    std::vector<Mat4> globals(n);
    composeGlobals(skeleton, pose, globals);
    globalsToPalette(skeleton, globals, out);
}

}  // namespace iron
