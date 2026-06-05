#include "asset/AnimationPlayer.h"

#include "asset/Pose.h"

#include <cmath>

namespace iron {

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
    Pose pose;
    if (clip_) {
        samplePose(*skeleton_, *clip_, time_, pose);
    } else {
        bindPose(*skeleton_, pose);
    }
    posePalette(*skeleton_, pose, out);
}

}  // namespace iron
