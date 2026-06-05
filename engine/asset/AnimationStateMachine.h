#pragma once

#include "asset/CharacterAnimator.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iron {

// A parameter-driven animation state machine layered over a CharacterAnimator.
// States are backed by animator states of the SAME name (a clip or a blend
// space already registered on the animator). Transitions fire automatically
// from float/bool parameters, cross-fading via the animator. This class owns
// NO pose math — it only decides WHEN to switchTo.
class AnimationStateMachine {
public:
    // A transition whose `from` equals this is evaluated regardless of the
    // current state ("from any state").
    static constexpr std::string_view kAnyState = "*";

    // Non-owning. The animator must outlive this state machine and is expected
    // to already have its clips / blend spaces / IK configured.
    explicit AnimationStateMachine(CharacterAnimator* animator);

    // Declare a state (backed by the animator state of the same name).
    void addState(std::string name);
    // While `state` is active, drive its blend space from float parameter
    // `floatParam` (calls animator->setBlendParam each frame).
    void bindBlendParam(std::string state, std::string floatParam);
    // Hard-cut into `name` immediately (fade 0).
    void setEntryState(std::string name);

    // Add a transition from `from` (or kAnyState) to `to`, cross-fading over
    // `fade` seconds. Returns a handle used to attach conditions. A transition
    // with no conditions is always ready (fires as soon as `from` is current).
    int addTransition(std::string from, std::string to, float fade);

    // AND-combined conditions on the transition `handle` (ignored if invalid).
    void whenFloatAbove(int handle, std::string param, float threshold);
    void whenFloatBelow(int handle, std::string param, float threshold);
    void whenBool(int handle, std::string param, bool expected);

    // Parameters (unset reads as 0.0f / false).
    void setFloat(std::string name, float value);
    void setBool(std::string name, bool value);

    // Per-frame: evaluate transitions (first ready wins, registration order),
    // forward the active state's bound blend param, then advance the animator.
    void update(float dt);
    // Delegate the skinning-palette write to the animator.
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

private:
    enum class CondKind { FloatAbove, FloatBelow, BoolEquals };
    struct Condition {
        CondKind    kind;
        std::string param;
        float       threshold = 0.0f;  // FloatAbove / FloatBelow
        bool        expected  = false; // BoolEquals
    };
    struct Transition {
        std::string from;
        std::string to;
        float       fade = 0.0f;
        std::vector<Condition> conditions;
    };

    bool conditionHolds(const Condition& c) const;
    bool transitionReady(const Transition& t) const;

    CharacterAnimator* animator_ = nullptr;
    std::vector<Transition> transitions_;
    std::unordered_map<std::string, std::string> blendBindings_;  // state -> float param
    std::unordered_map<std::string, float> floats_;
    std::unordered_map<std::string, bool>  bools_;
    std::unordered_set<std::string> states_;
    std::string currentState_;
};

}  // namespace iron
