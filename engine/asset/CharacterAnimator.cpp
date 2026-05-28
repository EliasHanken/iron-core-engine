#include "asset/CharacterAnimator.h"

#include "core/Log.h"

namespace iron {

void CharacterAnimator::setSkeleton(const Skeleton* skeleton) {
    player_.setSkeleton(skeleton);
    currentState_.clear();
    warnedUnknownState_ = false;
}

void CharacterAnimator::setClipForState(std::string state,
                                        const AnimationClip* clip) {
    clips_[std::move(state)] = clip;
}

void CharacterAnimator::switchTo(std::string_view state) {
    if (state == currentState_) return;  // no-op; keep advancing the clip

    auto it = clips_.find(std::string(state));
    if (it == clips_.end()) {
        if (!warnedUnknownState_) {
            Log::warn("CharacterAnimator: switchTo('%.*s') has no registered "
                      "clip; falling back to bind pose",
                      static_cast<int>(state.size()), state.data());
            warnedUnknownState_ = true;
        }
        player_.setClip(nullptr);
        currentState_.assign(state);
        return;
    }

    player_.setClip(it->second);  // setClip resets time to 0
    currentState_.assign(state);
}

void CharacterAnimator::update(float dt) {
    player_.update(dt);
}

void CharacterAnimator::evaluate(std::span<Mat4> out) const {
    player_.evaluate(out);
}

}  // namespace iron
