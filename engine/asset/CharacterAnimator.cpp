#include "asset/CharacterAnimator.h"

#include "asset/PoseBlend.h"
#include "core/Log.h"
#include "math/Vec.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace iron {

namespace {

// World position (translation column) of a global transform.
Vec3 originOf(const Mat4& m) {
    return Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
}

// Pivot-rotate `g` about world point `p` by quaternion `q`:  T(p) * R(q) * T(-p) * g.
Mat4 pivotRotate(const Mat4& g, Vec3 p, const Quat& q) {
    Mat4 piv = q.toMat4();
    const Vec4 rp = piv * Vec4{p.x, p.y, p.z, 1.0f};  // R*p (no translation in piv)
    piv.at(0, 3) = p.x - rp.x;
    piv.at(1, 3) = p.y - rp.y;
    piv.at(2, 3) = p.z - rp.z;
    return piv * g;
}

}  // namespace

void CharacterAnimator::setSkeleton(const Skeleton* skeleton) {
    skeleton_ = skeleton;
    currentState_.clear();
    time_ = 0.0f;
    fading_ = false;
    warnedUnknownState_ = false;
}

void CharacterAnimator::setClipForState(std::string state,
                                        const AnimationClip* clip) {
    clips_[std::move(state)] = clip;
}

void CharacterAnimator::setBlendSpaceForState(std::string state,
                                              BlendSpace1D space) {
    blendSpaces_[std::move(state)] = std::move(space);
}

void CharacterAnimator::setBlendParam(float param) {
    blendParam_ = param;
}

int CharacterAnimator::addTwoBoneIK(int rootBone, int midBone, int endBone,
                                    Vec3 pole, float weight) {
    twoBoneIK_.push_back(TwoBoneIK{rootBone, midBone, endBone, pole,
                                   Vec3{0, 0, 0}, weight, false});
    return static_cast<int>(twoBoneIK_.size()) - 1;
}
void CharacterAnimator::setIKTarget(int handle, Vec3 worldTarget) {
    if (handle < 0 || handle >= static_cast<int>(twoBoneIK_.size())) return;
    twoBoneIK_[handle].target = worldTarget;
    twoBoneIK_[handle].hasTarget = true;
}
void CharacterAnimator::setIKWeight(int handle, float weight) {
    if (handle < 0 || handle >= static_cast<int>(twoBoneIK_.size())) return;
    twoBoneIK_[handle].weight = weight;
}
int CharacterAnimator::addLookAt(int bone, Vec3 forwardAxis, float maxAngle,
                                 float weight) {
    lookAts_.push_back(LookAt{bone, forwardAxis, maxAngle, Vec3{0, 0, 0},
                              weight, false});
    return static_cast<int>(lookAts_.size()) - 1;
}
void CharacterAnimator::setLookAtTarget(int handle, Vec3 worldTarget) {
    if (handle < 0 || handle >= static_cast<int>(lookAts_.size())) return;
    lookAts_[handle].target = worldTarget;
    lookAts_[handle].hasTarget = true;
}
void CharacterAnimator::setLookAtWeight(int handle, float weight) {
    if (handle < 0 || handle >= static_cast<int>(lookAts_.size())) return;
    lookAts_[handle].weight = weight;
}

void CharacterAnimator::applyIK(std::span<Mat4> globals) const {
    if (!skeleton_) return;
    const int n = static_cast<int>(std::min(globals.size(),
                                            skeleton_->bones.size()));

    // returns true if `bone` is `ancestor` or a descendant of it.
    auto inSubtree = [&](int bone, int ancestor) {
        for (int b = bone, steps = 0; b >= 0 && steps < n;
             b = skeleton_->bones[b].parentIndex, ++steps) {
            if (b == ancestor) return true;
        }
        return false;
    };

    // Two-bone IK first (gross pose), then look-at (fine aim).
    for (const auto& ik : twoBoneIK_) {
        if (!ik.hasTarget || ik.weight <= 0.0f) continue;
        if (ik.root < 0 || ik.mid < 0 || ik.end < 0 ||
            ik.root >= n || ik.mid >= n || ik.end >= n) continue;
        const Vec3 root = originOf(globals[ik.root]);
        const Vec3 mid  = originOf(globals[ik.mid]);
        const Vec3 end  = originOf(globals[ik.end]);
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, ik.target, ik.pole);
        const Quat rootD = slerp(Quat::identity(), r.rootDelta,
                                 std::clamp(ik.weight, 0.0f, 1.0f));
        const Quat midD  = slerp(Quat::identity(), r.midDelta,
                                 std::clamp(ik.weight, 0.0f, 1.0f));
        // Rotate the root subtree about the root, then the mid subtree about
        // the (rotated) mid position.
        for (int b = 0; b < n; ++b)
            if (inSubtree(b, ik.root)) globals[b] = pivotRotate(globals[b], root, rootD);
        const Vec3 midAfter = originOf(globals[ik.mid]);
        for (int b = 0; b < n; ++b)
            if (inSubtree(b, ik.mid)) globals[b] = pivotRotate(globals[b], midAfter, midD);
    }

    for (const auto& la : lookAts_) {
        if (!la.hasTarget || la.weight <= 0.0f) continue;
        if (la.bone < 0 || la.bone >= n) continue;
        const Vec3 pos = originOf(globals[la.bone]);
        // Current world forward = global rotation applied to the local axis.
        const Vec4 fwd4 = globals[la.bone] * Vec4{la.forwardAxis.x,
                                                  la.forwardAxis.y,
                                                  la.forwardAxis.z, 0.0f};
        const Vec3 worldForward{fwd4.x, fwd4.y, fwd4.z};
        const Quat full = solveLookAt(pos, worldForward, la.target, la.maxAngle);
        const Quat d = slerp(Quat::identity(), full,
                             std::clamp(la.weight, 0.0f, 1.0f));
        for (int b = 0; b < n; ++b)
            if (inSubtree(b, la.bone)) globals[b] = pivotRotate(globals[b], pos, d);
    }
}

bool CharacterAnimator::isKnownState(std::string_view state) const {
    return clips_.count(std::string(state)) > 0 ||
           blendSpaces_.count(std::string(state)) > 0;
}

float CharacterAnimator::stateDuration(std::string_view state) const {
    auto bs = blendSpaces_.find(std::string(state));
    if (bs != blendSpaces_.end()) {
        return blendSpaceDuration(bs->second, blendParam_);
    }
    auto it = clips_.find(std::string(state));
    if (it == clips_.end() || !it->second) return 0.0f;
    return it->second->duration;
}

void CharacterAnimator::sampleState(std::string_view state, float time,
                                    Pose& out) const {
    auto bs = blendSpaces_.find(std::string(state));
    if (bs != blendSpaces_.end()) {
        if (!skeleton_) { out.bones.clear(); return; }
        sampleBlendSpace(*skeleton_, bs->second, blendParam_, time, out);
        return;
    }
    auto it = clips_.find(std::string(state));
    if (it == clips_.end() || !it->second) {
        if (skeleton_) bindPose(*skeleton_, out);
        else out.bones.clear();
        return;
    }
    samplePose(*skeleton_, *it->second, time, out);
}

void CharacterAnimator::switchTo(std::string_view state) {
    switchTo(state, 0.0f);
}

void CharacterAnimator::switchTo(std::string_view state, float fadeTime) {
    if (state == currentState_) return;  // no-op; keep advancing

    if (!isKnownState(state)) {
        if (!warnedUnknownState_) {
            Log::warn("CharacterAnimator: switchTo('%.*s') has no registered "
                      "clip; falling back to bind pose",
                      static_cast<int>(state.size()), state.data());
            warnedUnknownState_ = true;
        }
        currentState_.assign(state);
        time_ = 0.0f;
        fading_ = false;
        return;
    }

    const bool hardCut = fadeTime <= 0.0f || !skeleton_ || currentState_.empty();
    if (hardCut) {
        currentState_.assign(state);
        time_ = 0.0f;
        fading_ = false;
        return;
    }

    if (fading_) {
        // Mid-fade: freeze the in-progress blended pose as the new source. If
        // no pose has been evaluated yet (lastPose_ empty), fall back to the
        // current state's pose so the frozen source has the right bone count.
        prevFrozen_ = true;
        if (lastPose_.bones.empty()) {
            sampleState(currentState_, time_, prevFrozenPose_);
        } else {
            prevFrozenPose_ = lastPose_;
        }
    } else {
        prevFrozen_ = false;
        prevState_ = currentState_;
        prevTime_ = time_;
    }
    currentState_.assign(state);
    time_ = 0.0f;
    fading_ = true;
    fadeTime_ = fadeTime;
    fadeElapsed_ = 0.0f;
}

void CharacterAnimator::update(float dt) {
    if (!skeleton_) return;

    const float dur = stateDuration(currentState_);
    time_ += dt;
    if (dur > 0.0f) {
        time_ = std::fmod(time_, dur);
        if (time_ < 0.0f) time_ += dur;
    }

    if (fading_) {
        fadeElapsed_ += dt;
        if (!prevFrozen_) {
            const float pdur = stateDuration(prevState_);
            prevTime_ += dt;
            if (pdur > 0.0f) {
                prevTime_ = std::fmod(prevTime_, pdur);
                if (prevTime_ < 0.0f) prevTime_ += pdur;
            }
        }
        if (fadeElapsed_ >= fadeTime_) fading_ = false;
    }
}

void CharacterAnimator::evaluate(std::span<Mat4> out) const {
    if (!skeleton_) return;

    Pose cur;
    sampleState(currentState_, time_, cur);

    Pose pose;
    if (fading_ && fadeTime_ > 0.0f) {
        Pose prev;
        if (prevFrozen_) prev = prevFrozenPose_;
        else sampleState(prevState_, prevTime_, prev);
        const float w = std::clamp(fadeElapsed_ / fadeTime_, 0.0f, 1.0f);
        blendPose(prev, cur, w, pose);
    } else {
        pose = cur;
    }

    lastPose_ = pose;

    const std::size_t n = std::min({out.size(), skeleton_->bones.size(),
                                    pose.bones.size()});
    std::vector<Mat4> globals(n);
    composeGlobals(*skeleton_, pose, globals);
    applyIK(globals);
    globalsToPalette(*skeleton_, globals, out);
}

}  // namespace iron
