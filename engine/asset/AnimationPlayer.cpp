#include "asset/AnimationPlayer.h"

#include "math/Quaternion.h"
#include "math/Vec.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <vector>

namespace iron {

namespace {

// Composes a column-major TRS matrix: M = T * R * S.
//
// Starts from the rotation matrix, scales each basis column by the matching
// scale component, then drops translation into the last column. Equivalent
// to multiplying T*R*S out by hand, but avoids two full matmuls.
Mat4 composeTRS(Vec3 t, Quat r, Vec3 s) {
    Mat4 m = r.toMat4();
    m.at(0, 0) *= s.x; m.at(1, 0) *= s.x; m.at(2, 0) *= s.x;
    m.at(0, 1) *= s.y; m.at(1, 1) *= s.y; m.at(2, 1) *= s.y;
    m.at(0, 2) *= s.z; m.at(1, 2) *= s.z; m.at(2, 2) *= s.z;
    m.at(0, 3) = t.x; m.at(1, 3) = t.y; m.at(2, 3) = t.z;
    m.at(3, 3) = 1.0f;
    return m;
}

// Decompose a column-major Mat4 (assumed TRS, no shear) into T/R/S.
// Scale is recovered from basis column lengths; rotation is the matrix
// after dividing each column by its length, converted to a quaternion.
void decomposeTRS(const Mat4& m, Vec3& t, Quat& r, Vec3& s) {
    t = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    const Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = Vec3{length(c0), length(c1), length(c2)};

    Mat4 rot = Mat4::identity();
    if (s.x > 1e-8f) { rot.at(0, 0) = c0.x / s.x; rot.at(1, 0) = c0.y / s.x; rot.at(2, 0) = c0.z / s.x; }
    if (s.y > 1e-8f) { rot.at(0, 1) = c1.x / s.y; rot.at(1, 1) = c1.y / s.y; rot.at(2, 1) = c1.z / s.y; }
    if (s.z > 1e-8f) { rot.at(0, 2) = c2.x / s.z; rot.at(1, 2) = c2.y / s.z; rot.at(2, 2) = c2.z / s.z; }

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
}

// Returns index i such that inputs[i] <= t < inputs[i+1], or the last
// index if t is past the end, or 0 if t is before the start. Returns
// (size_t)-1 for an empty inputs vector — callers must handle.
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
    const Vec3 v1{s.outputs[(i + 1) * 3 + 0],
                  s.outputs[(i + 1) * 3 + 1],
                  s.outputs[(i + 1) * 3 + 2]};
    const float t0 = s.inputs[i];
    const float t1 = s.inputs[i + 1];
    const float a = (t - t0) / (t1 - t0);
    return Vec3{v0.x + (v1.x - v0.x) * a,
                v0.y + (v1.y - v0.y) * a,
                v0.z + (v1.z - v0.z) * a};
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
    const float t0 = s.inputs[i];
    const float t1 = s.inputs[i + 1];
    const float a = (t - t0) / (t1 - t0);
    return Quat::slerp(q0, q1, a);
}

}  // namespace

void AnimationPlayer::setSkeleton(const Skeleton* skeleton) {
    skeleton_ = skeleton;
    time_     = 0.0f;
}

void AnimationPlayer::setClip(const AnimationClip* clip) {
    clip_ = clip;
    time_ = 0.0f;
}

void AnimationPlayer::update(float dt) {
    if (!skeleton_ || !clip_ || clip_->duration <= 0.0f) return;
    time_ = std::fmod(time_ + dt, clip_->duration);
    if (time_ < 0.0f) time_ += clip_->duration;
}

void AnimationPlayer::evaluate(std::span<Mat4> out) const {
    if (!skeleton_) return;
    const std::size_t n = std::min(out.size(), skeleton_->bones.size());

    // Seed TRS from each bone's bind-pose transform; channels overwrite
    // whichever components they drive below.
    std::vector<Vec3> trans(n);
    std::vector<Quat> rot(n);
    std::vector<Vec3> scale(n);
    for (std::size_t i = 0; i < n; ++i) {
        decomposeTRS(skeleton_->bones[i].localBindTransform,
                     trans[i], rot[i], scale[i]);
    }

    if (clip_) {
        for (const auto& ch : clip_->channels) {
            if (ch.targetBone < 0 ||
                static_cast<std::size_t>(ch.targetBone) >= n) continue;
            if (ch.samplerIndex < 0 ||
                static_cast<std::size_t>(ch.samplerIndex) >= clip_->samplers.size()) continue;
            const AnimationSampler& s = clip_->samplers[ch.samplerIndex];
            switch (ch.path) {
                case AnimationPath::Translation:
                    trans[ch.targetBone] = sampleVec3(s, time_);
                    break;
                case AnimationPath::Rotation:
                    rot[ch.targetBone] = sampleQuat(s, time_);
                    break;
                case AnimationPath::Scale:
                    scale[ch.targetBone] = sampleVec3(s, time_);
                    break;
            }
        }
    }

    // Walk hierarchy parent-before-child. Skeleton bones must be sorted so
    // each bone's parent has a lower index — the loader guarantees this.
    std::vector<Mat4> globals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4 localMat = composeTRS(trans[i], rot[i], scale[i]);
        const int parent = skeleton_->bones[i].parentIndex;
        if (parent < 0) {
            globals[i] = localMat;
        } else {
            assert(static_cast<std::size_t>(parent) < i &&
                   "bone parent must have lower index");
            globals[i] = globals[parent] * localMat;
        }
        out[i] = globals[i] * skeleton_->bones[i].inverseBindMatrix;
    }
}

}  // namespace iron
