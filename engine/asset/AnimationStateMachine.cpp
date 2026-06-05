#include "asset/AnimationStateMachine.h"

#include "core/Log.h"

namespace iron {

AnimationStateMachine::AnimationStateMachine(CharacterAnimator* animator)
    : animator_(animator) {}

void AnimationStateMachine::addState(std::string name) {
    states_.insert(std::move(name));
}

void AnimationStateMachine::bindBlendParam(std::string state,
                                           std::string floatParam) {
    if (!states_.count(state)) {
        Log::warn("AnimationStateMachine: bindBlendParam references unknown "
                  "state '%s'", state.c_str());
    }
    blendBindings_[std::move(state)] = std::move(floatParam);
}

void AnimationStateMachine::setEntryState(std::string name) {
    if (!states_.count(name)) {
        Log::warn("AnimationStateMachine: setEntryState references unknown "
                  "state '%s'", name.c_str());
    }
    currentState_ = std::move(name);
    if (animator_) animator_->switchTo(currentState_, 0.0f);
}

int AnimationStateMachine::addTransition(std::string from, std::string to,
                                         float fade) {
    if (from != kAnyState && !states_.count(from)) {
        Log::warn("AnimationStateMachine: transition 'from' references unknown "
                  "state '%s'", from.c_str());
    }
    if (!states_.count(to)) {
        Log::warn("AnimationStateMachine: transition 'to' references unknown "
                  "state '%s'", to.c_str());
    }
    transitions_.push_back(Transition{std::move(from), std::move(to), fade, {}});
    return static_cast<int>(transitions_.size()) - 1;
}

void AnimationStateMachine::whenFloatAbove(int handle, std::string param,
                                           float threshold) {
    if (handle < 0 || handle >= static_cast<int>(transitions_.size())) return;
    transitions_[handle].conditions.push_back(
        Condition{CondKind::FloatAbove, std::move(param), threshold, false});
}

void AnimationStateMachine::whenFloatBelow(int handle, std::string param,
                                           float threshold) {
    if (handle < 0 || handle >= static_cast<int>(transitions_.size())) return;
    transitions_[handle].conditions.push_back(
        Condition{CondKind::FloatBelow, std::move(param), threshold, false});
}

void AnimationStateMachine::whenBool(int handle, std::string param,
                                     bool expected) {
    if (handle < 0 || handle >= static_cast<int>(transitions_.size())) return;
    transitions_[handle].conditions.push_back(
        Condition{CondKind::BoolEquals, std::move(param), 0.0f, expected});
}

void AnimationStateMachine::setFloat(std::string name, float value) {
    floats_[std::move(name)] = value;
}

void AnimationStateMachine::setBool(std::string name, bool value) {
    bools_[std::move(name)] = value;
}

bool AnimationStateMachine::conditionHolds(const Condition& c) const {
    switch (c.kind) {
        case CondKind::FloatAbove: {
            auto it = floats_.find(c.param);
            const float v = (it != floats_.end()) ? it->second : 0.0f;
            return v > c.threshold;
        }
        case CondKind::FloatBelow: {
            auto it = floats_.find(c.param);
            const float v = (it != floats_.end()) ? it->second : 0.0f;
            return v < c.threshold;
        }
        case CondKind::BoolEquals: {
            auto it = bools_.find(c.param);
            const bool v = (it != bools_.end()) ? it->second : false;
            return v == c.expected;
        }
    }
    return false;
}

bool AnimationStateMachine::transitionReady(const Transition& t) const {
    if (t.to == currentState_) return false;
    if (t.from != kAnyState && t.from != currentState_) return false;
    for (const auto& c : t.conditions) {
        if (!conditionHolds(c)) return false;
    }
    return true;
}

void AnimationStateMachine::update(float dt) {
    if (!animator_) return;

    for (const auto& t : transitions_) {
        if (transitionReady(t)) {
            animator_->switchTo(t.to, t.fade);
            currentState_ = t.to;
            break;
        }
    }

    auto bit = blendBindings_.find(currentState_);
    if (bit != blendBindings_.end()) {
        auto fit = floats_.find(bit->second);
        animator_->setBlendParam((fit != floats_.end()) ? fit->second : 0.0f);
    }

    animator_->update(dt);
}

void AnimationStateMachine::evaluate(std::span<Mat4> out) const {
    if (animator_) animator_->evaluate(out);
}

}  // namespace iron
