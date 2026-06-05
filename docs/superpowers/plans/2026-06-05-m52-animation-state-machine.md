# M52 — Animation State Machine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a parameter-driven animation state machine on top of M51's `CharacterAnimator` — named states, transitions with conditions, automatic cross-fades — so gameplay sets parameters instead of imperatively calling `switchTo` each frame.

**Architecture:** A new `AnimationStateMachine` class holds a **non-owning `CharacterAnimator*`** and owns transition logic + a parameter store only (no pose math). Each frame it evaluates transitions (first satisfied wins, registration order), forwards a bound blend parameter for the active state, then advances the animator. The animator does all cross-fade / blend-space / IK work exactly as in M51.

**Tech Stack:** C++17, existing `iron::CharacterAnimator` (M51), `core/Log.h` for setup-time warnings, the `test_framework.h` CHECK/CHECK_NEAR harness, CMake/CTest. Canonical build dir is `build-vk` (Vulkan); use `-C Debug` with ctest (multi-config generator).

**Spec:** `docs/superpowers/specs/2026-06-05-m52-animation-state-machine-design.md`

**Conventions for every commit:** end the commit message body with
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push (PR handled at the end). Branch is `m52-animation-state-machine` (stacked on `m51-animation-blending-ik`).

---

## File Structure

**New engine files:**
- `engine/asset/AnimationStateMachine.h` / `.cpp` — the entire feature (states, params, transitions, per-frame evaluation, delegation to `CharacterAnimator`).

**Modified engine files:**
- `engine/CMakeLists.txt` — register `asset/AnimationStateMachine.cpp`.

**New tests:**
- `tests/test_animation_state_machine.cpp` — registered in `tests/CMakeLists.txt`.

**Demo:**
- `games/07-net-shooter/main.cpp` — drive a per-peer `AnimationStateMachine` instead of hand-written `switchTo`.

---

## Task 1: AnimationStateMachine core + initial tests

**Files:**
- Create: `engine/asset/AnimationStateMachine.h`
- Create: `engine/asset/AnimationStateMachine.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/test_animation_state_machine.cpp`

- [ ] **Step 1: Create `engine/asset/AnimationStateMachine.h`**

```cpp
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
```

- [ ] **Step 2: Write the failing test `tests/test_animation_state_machine.cpp`**

```cpp
#include "asset/AnimationStateMachine.h"
#include "asset/Animation.h"
#include "asset/CharacterAnimator.h"
#include "asset/PoseBlend.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "test_framework.h"

#include <array>

using namespace iron;

namespace {

Skeleton makeTrivialSkeleton() {
    Skeleton s;
    Bone b;
    b.parentIndex = -1;
    b.inverseBindMatrix = Mat4::identity();
    b.localBindTransform = Mat4::identity();
    b.name = "root";
    s.bones.push_back(b);
    return s;
}

// Bone 0 translates x: 0 -> 10 over 1s.
AnimationClip makeTranslateClip(const char* name) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs  = {0.0f, 1.0f};
    s.outputs = {0, 0, 0,  10, 0, 0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch;
    ch.targetBone = 0;
    ch.path = AnimationPath::Translation;
    ch.samplerIndex = 0;
    c.channels.push_back(ch);
    return c;
}

// Bone 0 stays at x=0 (dur 1s) — a "still" clip.
AnimationClip makeStillClip(const char* name) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs  = {0.0f, 1.0f};
    s.outputs = {0, 0, 0,  0, 0, 0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch;
    ch.targetBone = 0;
    ch.path = AnimationPath::Translation;
    ch.samplerIndex = 0;
    c.channels.push_back(ch);
    return c;
}

}  // namespace

int main() {
    // Entry state is active and plays its clip.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("walk", &walk);

        AnimationStateMachine sm(&anim);
        sm.addState("walk");
        sm.setEntryState("walk");
        CHECK(sm.currentState() == "walk");

        sm.update(0.5f);
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);
    }

    // FloatAbove transition fires when the parameter crosses the threshold.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip idle = makeStillClip("idle");
        const AnimationClip run  = makeTranslateClip("run");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("idle", &idle);
        anim.setClipForState("run",  &run);

        AnimationStateMachine sm(&anim);
        sm.addState("idle");
        sm.addState("run");
        sm.setEntryState("idle");
        const int t = sm.addTransition("idle", "run", 0.0f);  // hard cut
        sm.whenFloatAbove(t, "speed", 2.5f);

        // Below threshold: stays idle.
        sm.setFloat("speed", 1.0f);
        sm.update(0.1f);
        CHECK(sm.currentState() == "idle");

        // Above threshold: transitions to run.
        sm.setFloat("speed", 3.0f);
        sm.update(0.5f);
        CHECK(sm.currentState() == "run");
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);  // run hard-cut to 0, then 0.5s -> x=5
    }

    // BoolEquals transition.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip ground = makeStillClip("ground");
        const AnimationClip air    = makeStillClip("air");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("ground", &ground);
        anim.setClipForState("air", &air);

        AnimationStateMachine sm(&anim);
        sm.addState("ground");
        sm.addState("air");
        sm.setEntryState("ground");
        const int t = sm.addTransition("ground", "air", 0.0f);
        sm.whenBool(t, "grounded", false);

        sm.setBool("grounded", true);
        sm.update(0.1f);
        CHECK(sm.currentState() == "ground");
        sm.setBool("grounded", false);
        sm.update(0.1f);
        CHECK(sm.currentState() == "air");
    }

    // Bound blend parameter drives the active state's blend space.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip still = makeStillClip("still");
        const AnimationClip walk  = makeTranslateClip("walk");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        BlendSpace1D loco;
        loco.add(0.0f, &still);  // param 0 -> x stays 0
        loco.add(1.0f, &walk);   // param 1 -> x up to 10
        anim.setBlendSpaceForState("loco", std::move(loco));

        AnimationStateMachine sm(&anim);
        sm.addState("loco");
        sm.bindBlendParam("loco", "speed");
        sm.setEntryState("loco");

        sm.setFloat("speed", 0.5f);
        sm.update(0.5f);  // forwards setBlendParam(0.5); time -> 0.5
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        // blendSpace(param 0.5) at phase 0.5: blend(still x=0, walk x=5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register source + test, then build to confirm the test fails**

In `engine/CMakeLists.txt`, add `asset/AnimationStateMachine.cpp` immediately after the `asset/CharacterAnimator.cpp` line:
```cmake
  asset/CharacterAnimator.cpp
  asset/AnimationStateMachine.cpp
```
In `tests/CMakeLists.txt`, add after the `iron_add_test(test_character_animator ...)` line:
```cmake
iron_add_test(test_animation_state_machine test_animation_state_machine.cpp)
```
Run: `cmake --build build-vk --config Debug --target test_animation_state_machine`
Expected: FAIL — link errors (AnimationStateMachine members undefined).

- [ ] **Step 4: Implement `engine/asset/AnimationStateMachine.cpp`**

```cpp
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
    if (animator_) animator_->switchTo(currentState_, 0.0f);  // hard cut
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

    // 1. Evaluate transitions: first ready wins, registration order; one fires.
    for (const auto& t : transitions_) {
        if (transitionReady(t)) {
            animator_->switchTo(t.to, t.fade);
            currentState_ = t.to;
            break;
        }
    }

    // 2. Forward the bound blend param for the (possibly new) current state.
    auto bit = blendBindings_.find(currentState_);
    if (bit != blendBindings_.end()) {
        auto fit = floats_.find(bit->second);
        animator_->setBlendParam((fit != floats_.end()) ? fit->second : 0.0f);
    }

    // 3. Advance the animator.
    animator_->update(dt);
}

void AnimationStateMachine::evaluate(std::span<Mat4> out) const {
    if (animator_) animator_->evaluate(out);
}

}  // namespace iron
```

- [ ] **Step 5: Build and run the test**

Run: `cmake --build build-vk --config Debug --target test_animation_state_machine && ctest --test-dir build-vk -C Debug -R test_animation_state_machine --output-on-failure`
Expected: PASS — "OK - all checks passed".

- [ ] **Step 6: Commit**

```bash
git add engine/asset/AnimationStateMachine.h engine/asset/AnimationStateMachine.cpp engine/CMakeLists.txt tests/test_animation_state_machine.cpp tests/CMakeLists.txt
git commit -m "M52: AnimationStateMachine core (param-driven transitions over CharacterAnimator)"
```

---

## Task 2: Transition-semantics coverage tests

The logic already exists (Task 1). This task locks the edge-case behavior with more tests. No production changes.

**Files:**
- Modify: `tests/test_animation_state_machine.cpp`

- [ ] **Step 1: Add edge-case tests before `return iron_test_result();`**

```cpp
    // AND-combined conditions: BOTH must hold.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip a = makeStillClip("a");
        const AnimationClip b = makeStillClip("b");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("a", &a);
        anim.setClipForState("b", &b);

        AnimationStateMachine sm(&anim);
        sm.addState("a");
        sm.addState("b");
        sm.setEntryState("a");
        const int t = sm.addTransition("a", "b", 0.0f);
        sm.whenFloatAbove(t, "speed", 2.5f);
        sm.whenBool(t, "armed", true);

        sm.setFloat("speed", 3.0f);
        sm.setBool("armed", false);     // only one condition holds
        sm.update(0.0f);
        CHECK(sm.currentState() == "a");
        sm.setBool("armed", true);      // now both hold
        sm.update(0.0f);
        CHECK(sm.currentState() == "b");
    }

    // kAnyState transition fires regardless of the current state.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip a   = makeStillClip("a");
        const AnimationClip hit = makeStillClip("hit");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("a", &a);
        anim.setClipForState("hit", &hit);

        AnimationStateMachine sm(&anim);
        sm.addState("a");
        sm.addState("hit");
        sm.setEntryState("a");
        const int t = sm.addTransition(std::string(AnimationStateMachine::kAnyState),
                                       "hit", 0.0f);
        sm.whenBool(t, "hit", true);

        sm.setBool("hit", true);
        sm.update(0.0f);
        CHECK(sm.currentState() == "hit");
    }

    // Registration order breaks ties: the first ready transition wins.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip a = makeStillClip("a");
        const AnimationClip b = makeStillClip("b");
        const AnimationClip c = makeStillClip("c");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("a", &a);
        anim.setClipForState("b", &b);
        anim.setClipForState("c", &c);

        AnimationStateMachine sm(&anim);
        sm.addState("a");
        sm.addState("b");
        sm.addState("c");
        sm.setEntryState("a");
        const int t1 = sm.addTransition("a", "b", 0.0f);  // registered FIRST
        sm.whenBool(t1, "go", true);
        const int t2 = sm.addTransition("a", "c", 0.0f);  // also ready
        sm.whenBool(t2, "go", true);

        sm.setBool("go", true);
        sm.update(0.0f);
        CHECK(sm.currentState() == "b");  // first wins, and only one fires
    }

    // A transition whose target is the current state never fires.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip a = makeStillClip("a");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("a", &a);

        AnimationStateMachine sm(&anim);
        sm.addState("a");
        sm.setEntryState("a");
        const int t = sm.addTransition("a", "a", 0.0f);
        sm.whenBool(t, "go", true);
        sm.setBool("go", true);
        sm.update(0.0f);
        CHECK(sm.currentState() == "a");  // no spurious change / no crash
    }

    // A conditionless transition fires as soon as `from` is current.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip a = makeStillClip("a");
        const AnimationClip b = makeStillClip("b");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("a", &a);
        anim.setClipForState("b", &b);

        AnimationStateMachine sm(&anim);
        sm.addState("a");
        sm.addState("b");
        sm.setEntryState("a");
        sm.addTransition("a", "b", 0.0f);  // no conditions
        sm.update(0.0f);
        CHECK(sm.currentState() == "b");
    }

    // FloatBelow transition.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip run  = makeStillClip("run");
        const AnimationClip idle = makeStillClip("idle");
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("run", &run);
        anim.setClipForState("idle", &idle);

        AnimationStateMachine sm(&anim);
        sm.addState("run");
        sm.addState("idle");
        sm.setEntryState("run");
        const int t = sm.addTransition("run", "idle", 0.0f);
        sm.whenFloatBelow(t, "speed", 0.3f);

        sm.setFloat("speed", 1.0f);
        sm.update(0.0f);
        CHECK(sm.currentState() == "run");
        sm.setFloat("speed", 0.1f);
        sm.update(0.0f);
        CHECK(sm.currentState() == "idle");
    }

    // Fade time is passed through to the animator's cross-fade.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip stand = makeStillClip("stand");  // x stays 0
        const AnimationClip run   = makeTranslateClip("run"); // x:0->10
        CharacterAnimator anim;
        anim.setSkeleton(&sk);
        anim.setClipForState("stand", &stand);
        anim.setClipForState("run", &run);

        AnimationStateMachine sm(&anim);
        sm.addState("stand");
        sm.addState("run");
        sm.setEntryState("stand");
        const int t = sm.addTransition("stand", "run", 1.0f);  // 1s fade
        sm.whenBool(t, "go", true);

        sm.update(0.1f);            // settle in stand
        sm.setBool("go", true);
        sm.update(0.5f);            // fire + advance: fade weight 0.5, run t=0.5 -> x=5
        std::array<Mat4, 1> out{};
        sm.evaluate(out);
        // blend(prev stand x=0, cur run x=5, w=0.5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-vk --config Debug --target test_animation_state_machine && ctest --test-dir build-vk -C Debug -R test_animation_state_machine --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Full build + full test sweep (interface-add gate)**

Run: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: ALL targets build, ALL tests pass (existing 68 + the new one).

- [ ] **Step 4: Commit**

```bash
git add tests/test_animation_state_machine.cpp
git commit -m "M52: cover AND/anyState/order/no-op/conditionless/fade transition semantics"
```

---

## Task 3: Net-shooter demo — drive a per-peer state machine

Replace the hand-written `switchTo` in `submitPlayerFox` with an `AnimationStateMachine`. No unit test; validated at the visual gate. The discrete footstep-state return and the M51 head look-at are preserved unchanged.

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Add a per-peer state-machine map next to `playerAnimators`**

Find the declaration `std::unordered_map<std::uint32_t, iron::CharacterAnimator> playerAnimators;` (~line 661) and add below it:
```cpp
    std::unordered_map<std::uint32_t, iron::AnimationStateMachine> playerStateMachines;
```
Add the include near the top with the other engine includes (search for `#include "asset/CharacterAnimator.h"` and add after it):
```cpp
#include "asset/AnimationStateMachine.h"
```
> `std::unordered_map` nodes are stable across rehash, so `&playerAnimators[pid]` stays valid for the lifetime of the entry — the state machine can hold that pointer safely.

- [ ] **Step 2: Erase the state machine on peer-left**

In the `setOnPeerLeft` lambda (search for `playerAnimators.erase(pid);`), add immediately after it:
```cpp
        playerStateMachines.erase(pid);
```

- [ ] **Step 3: Build the state machine on first sight, inside `submitPlayerFox`'s `if (inserted)` block**

The `if (inserted)` block currently sets the skeleton, the `"locomotion"` blend space, the `"idle"` clip, and the head look-at. AFTER that existing setup (still inside `if (inserted)`), construct the state machine for this peer:
```cpp
                // M52 — drive transitions with a state machine instead of
                // hand-written switchTo. States reuse the animator's
                // "locomotion" (speed-blended) and "idle" (in-air) states.
                auto [smIt, smInserted] = playerStateMachines.try_emplace(
                    pid, &animIt->second);
                iron::AnimationStateMachine& sm = smIt->second;
                sm.addState("locomotion");
                sm.addState("idle");
                sm.bindBlendParam("locomotion", "speed");
                sm.setEntryState("locomotion");
                const int toAir = sm.addTransition("locomotion", "idle", 0.15f);
                sm.whenBool(toAir, "grounded", false);
                const int toGround = sm.addTransition("idle", "locomotion", 0.15f);
                sm.whenBool(toGround, "grounded", true);
```

- [ ] **Step 4: Replace the per-frame `switchTo`/`setBlendParam`/`update` with state-machine parameter feeding**

The current per-frame block (after the `footstepState` computation) reads roughly:
```cpp
            auto& anim = animIt->second;
            if (!grounded) {
                anim.switchTo("idle", 0.15f);
            } else {
                anim.switchTo("locomotion", 0.15f);
                anim.setBlendParam(speed);
            }
```
Replace that block with (feed the SM's parameters; the SM owns the transitions + blend param):
```cpp
            auto& anim = animIt->second;
            iron::AnimationStateMachine& sm = playerStateMachines.at(pid);
            sm.setFloat("speed", speed);
            sm.setBool("grounded", grounded);
```
Leave the `foxModelMat` computation and the look-at target block (which calls `anim.setLookAtTarget(...)`) exactly as they are — they set the IK target on the animator before evaluation.

- [ ] **Step 5: Drive the animator through the state machine**

Find the per-frame `anim.update(frameDt);` and the `anim.evaluate(...)` call in `submitPlayerFox` and route them through the state machine instead. Replace:
```cpp
            anim.update(frameDt);
```
with:
```cpp
            sm.update(frameDt);
```
And replace the evaluate call (currently `anim.evaluate(std::span<iron::Mat4>(bones.data(), std::min(n, bones.size())));`) with:
```cpp
            sm.evaluate(std::span<iron::Mat4>(bones.data(),
                                              std::min(n, bones.size())));
```
> Do NOT also call `anim.update`/`anim.evaluate` — `sm.update` already advances the animator and `sm.evaluate` delegates to it. The look-at target set earlier on `anim` is consumed by `sm.evaluate` (which runs the animator's IK).

- [ ] **Step 6: Build the game**

Run: `cmake --build build-vk --config Debug --target net-shooter`
Expected: links cleanly.

- [ ] **Step 7: Full build + sweep (no regressions)**

Run: `cmake --build build-vk --config Debug && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all green.

- [ ] **Step 8: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M52: net-shooter foxes driven by an AnimationStateMachine (declarative transitions)"
```

---

## Task 4: Update progress memory

**Files:**
- Modify: `C:\Users\elias\.claude\projects\C--Users-elias-Documents--dev-iron-core-engine\memory\iron-core-engine-progress.md`
- Modify: `C:\Users\elias\.claude\projects\C--Users-elias-Documents--dev-iron-core-engine\memory\MEMORY.md`

- [ ] **Step 1: Record M52 after the PR is opened**

Append an M52 entry to the progress memory (AnimationStateMachine summary, files, the stacked-on-M51 relationship, PR number once known) and add/refresh a one-line pointer in `MEMORY.md`. Documentation only; no code. Do this after the PR is opened so the PR number is real. Memory files live outside the repo — no git commit needed.

---

## Visual Gate (deferred to the user's return)

Run the net-shooter and confirm the foxes behave the SAME as the M51 demo (the state machine should be behavior-preserving): smooth speed-blended gait, cross-fade idle↔locomotion on jump/land, head still tracks the viewer. The state machine adds no new visible behavior — it's an architecture change — so "no regression vs M51" is the bar.

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- Separate `AnimationStateMachine` over non-owning `CharacterAnimator*` → Task 1. ✓
- States, param store, transitions with FloatAbove/FloatBelow/BoolEquals, AND-combined → Task 1 (+ Task 2 coverage). ✓
- kAnyState transitions, registration-order priority, one-per-update, conditionless, to-current ignored → Task 1 logic, Task 2 tests. ✓
- Bound blend param forwarding (1D) → Task 1 (`bindBlendParam` + `update` step 2), test in Task 1. ✓
- Entry state hard-cut → Task 1 (`setEntryState`). ✓
- Per-frame algorithm (evaluate → forward blend param → advance) → Task 1 `update`. ✓
- Demo rewiring preserving footstep return + look-at → Task 3. ✓
- Out-of-scope items (exit-time, events, nested SMs, 2D binding, serialized graph) not implemented. ✓

**2. Placeholder scan:** No TBD/TODO. Demo edit steps reference concrete existing anchors (the `if (inserted)` block, the `footstepState`/`switchTo` block, `anim.update`/`anim.evaluate`) from the M51 demo; the implementer must read the current lambda and apply the replacements there.

**3. Type consistency:** `AnimationStateMachine` ctor `(CharacterAnimator*)`, `addState`/`bindBlendParam`/`setEntryState`/`addTransition`/`whenFloatAbove`/`whenFloatBelow`/`whenBool`/`setFloat`/`setBool`/`update`/`evaluate`/`currentState`/`kAnyState` are used consistently across Tasks 1–3. Reuses M51's real `CharacterAnimator` API (`setSkeleton`/`setClipForState`/`setBlendSpaceForState`/`setBlendParam`/`switchTo(string_view,float)`/`update`/`evaluate`) and `BlendSpace1D::add`, all verified against the M51 headers. ✓
