#include "asset/CharacterAnimator.h"

#include "asset/PoseBlend.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace iron {

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

bool CharacterAnimator::isKnownState(std::string_view state) const {
    return clips_.count(std::string(state)) > 0 ||
           blendSpaces_.count(std::string(state)) > 0;
}

float CharacterAnimator::stateDuration(std::string_view state) const {
    auto bs = blendSpaces_.find(std::string(state));
    if (bs != blendSpaces_.end()) {
        for (const auto& s : bs->second.samples) {
            if (s.second && s.second->duration > 0.0f) return s.second->duration;
        }
        return 0.0f;
    }
    auto it = clips_.find(std::string(state));
    if (it == clips_.end() || !it->second) return 0.0f;
    return it->second->duration;
}

void CharacterAnimator::sampleState(std::string_view state, float time,
                                    Pose& out) const {
    auto bs = blendSpaces_.find(std::string(state));
    if (bs != blendSpaces_.end()) {
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
    posePalette(*skeleton_, pose, out);
}

}  // namespace iron
