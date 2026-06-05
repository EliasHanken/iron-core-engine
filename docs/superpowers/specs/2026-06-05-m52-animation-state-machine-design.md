# M52 — Animation State Machine — Design

> **Status:** Brainstormed autonomously while the user was away (user delegated this as a "build-to-open-PR" task). Every scoping decision is flagged under **Assumptions / decisions to review** so the user can veto any of them in the PR review. The open PR is the review gate; the visual gate (net-shooter) is deferred until the user is back.

> **Stacks on:** M51 (animation blending + IK), branch `m51-animation-blending-ik` — needs `CharacterAnimator`'s `switchTo(name, fade)`, blend spaces, and `setBlendParam`. M51 is code-complete + reviewed (pending only its own visual gate).

## Goal

Replace the ad-hoc, hand-written `switchTo(...)` calls in gameplay with a real **animation state machine** (the Unreal AnimGraph / Unity Animator Controller pattern): named states, parameter-driven transitions with conditions, and automatic cross-fades. This turns M51's blending + IK into something usable from game code by setting parameters instead of imperatively choosing clips each frame.

## Where it sits

The state machine is a **thin, separate layer on top of `CharacterAnimator`** — it owns no pose math. It decides *when* to transition; `CharacterAnimator` does the cross-fade, blend-space sampling, and IK exactly as in M51.

```
game code → AnimationStateMachine (transitions, params)
                   │ switchTo(target, fade) / setBlendParam(...)
                   ▼
            CharacterAnimator (M51: cross-fade + blend space + IK)
                   │ evaluate(palette)
                   ▼
            renderer (span<Mat4>)
```

`AnimationStateMachine` holds a **non-owning `CharacterAnimator*`** (consistent with how `CharacterAnimator` holds non-owning skeleton/clip pointers). Game code configures the animator as it does today (register clips / blend spaces / IK), then builds a state machine over it.

## Public interface (sketch)

```cpp
namespace iron {

class AnimationStateMachine {
public:
    explicit AnimationStateMachine(CharacterAnimator* animator);

    // A state is backed by an animator state of the SAME name (a clip or a
    // blend space already registered on the CharacterAnimator).
    void addState(std::string name);
    // Optionally drive this state's blend space from a float parameter while
    // the state is active (e.g. bind "locomotion" -> "speed").
    void bindBlendParam(std::string state, std::string floatParam);
    // Hard-cut into the entry state immediately.
    void setEntryState(std::string name);

    // A transition fires when ALL its conditions hold and target != current,
    // cross-fading over `fade` seconds. `from == kAnyState` ("*") means the
    // transition is evaluated regardless of the current state. Returns a handle
    // used to attach conditions.
    int addTransition(std::string from, std::string to, float fade);

    // Conditions on the most-recently-added transition (by handle). AND-combined.
    void whenFloatAbove(int transition, std::string param, float threshold);
    void whenFloatBelow(int transition, std::string param, float threshold);
    void whenBool(int transition, std::string param, bool expected);

    // Parameters (default 0.0f / false until set).
    void setFloat(std::string name, float value);
    void setBool(std::string name, bool value);

    // Per-frame: evaluate transitions, forward the active state's bound blend
    // param, then advance the underlying animator by dt.
    void update(float dt);
    // Delegates to the CharacterAnimator.
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const;

    static constexpr std::string_view kAnyState = "*";
};

}  // namespace iron
```

Handle-based condition attachment mirrors M51's `addTwoBoneIK`/`addLookAt` handle pattern, keeping the codebase consistent.

## Per-frame algorithm (`update(dt)`)

1. **Evaluate transitions.** Iterate all transitions once in **registration order**. A transition is a candidate if `from == currentState_` or `from == kAnyState`. Fire the **first** candidate whose conditions all hold and whose `to != currentState_`: call `animator_->switchTo(to, fade)` and set `currentState_ = to`. At most one transition fires per `update`. (Registration order is the single, total priority — a state-specific and an any-state transition compete purely by when they were added; register the higher-priority one first.)
2. **Forward the bound blend param.** If `currentState_` has a bound float parameter, call `animator_->setBlendParam(params_[bound])`. (Done after step 1 so a freshly-entered blend-space state gets its parameter this same frame.)
3. **Advance.** `animator_->update(dt)`.

Continuous evaluation every frame is intentional (matches Unity). Re-firing a transition to the state we are already fading into is a no-op because `currentState_` already equals that target. Mid-fade re-targets (conditions flip during a fade) are handled by M51's `CharacterAnimator` (it freezes the in-progress blended pose), so transitions can interrupt cleanly.

## Conditions

A condition is one of:
- **FloatAbove** — `params_.float[param] > threshold`
- **FloatBelow** — `params_.float[param] < threshold`
- **BoolEquals** — `params_.bool[param] == expected`

A transition's conditions are **AND-combined**. "OR" is expressed by adding multiple transitions to the same target. Unset parameters read as `0.0f` / `false`.

## Example (net-shooter foxes)

```cpp
CharacterAnimator anim;                       // configured as in M51
anim.setBlendSpaceForState("locomotion", gait);
anim.setClipForState("idle", foxIdleClip);

AnimationStateMachine sm(&anim);
sm.addState("locomotion");
sm.addState("idle");
sm.bindBlendParam("locomotion", "speed");     // gait driven by speed param
sm.setEntryState("locomotion");

int toAir = sm.addTransition("locomotion", "idle", 0.15f);
sm.whenBool(toAir, "grounded", false);
int toGround = sm.addTransition("idle", "locomotion", 0.15f);
sm.whenBool(toGround, "grounded", true);

// each frame:
sm.setFloat("speed", horizontalSpeed);
sm.setBool("grounded", grounded);
sm.update(dt);
sm.evaluate(boneMatrices);
```

This replaces the hand-written `if (!grounded) switchTo("idle",…) else { switchTo("locomotion",…); setBlendParam(speed); }` in `submitPlayerFox` with declarative states/transitions. The head look-at IK from M51 stays configured directly on the `CharacterAnimator` and is unaffected.

## Files

**New engine:**
- `engine/asset/AnimationStateMachine.h` / `.cpp` — the whole feature. Small, focused, one responsibility (transition logic + delegation). No pose math.

**New test:**
- `tests/test_animation_state_machine.cpp` — registered in `tests/CMakeLists.txt`.

**Modified:**
- `engine/CMakeLists.txt` — register `asset/AnimationStateMachine.cpp`.
- `games/07-net-shooter/main.cpp` — rewire `submitPlayerFox` to drive a per-peer `AnimationStateMachine` instead of manual `switchTo`. (Demo only; visual-gated later.)

## Testing strategy

Transition logic is pure and deterministic — unit-tested against a real `CharacterAnimator` driving trivial 1-bone clips, observing `currentState()` and the `evaluate()` palette output:

- Entry state is active after `setEntryState`.
- FloatAbove / FloatBelow / BoolEquals each fire correctly; unmet conditions do not fire.
- AND-combined conditions require all to hold.
- `kAnyState` transition fires from any current state.
- Registration order breaks ties (first satisfied transition wins).
- A transition to the current state is ignored (no spurious reset).
- Bound blend param is forwarded (observe via blended `evaluate()` output: param drives the gait blend).
- Cross-fade fade time is passed through (observe a mid-fade blended pose).
- At most one transition fires per `update`.

The underlying cross-fade / blend-space / IK behavior is already locked by M51's tests; M52 only adds the transition-decision layer.

## Assumptions / decisions to review (the user can veto any of these in the PR)

1. **Separate `AnimationStateMachine` class over a non-owning `CharacterAnimator*`** — not baked into `CharacterAnimator`. Keeps pose math and transition logic in separate, independently testable units. *(Alt: bake `addState/addTransition` into `CharacterAnimator`.)*
2. **Conditions limited to FloatAbove / FloatBelow / BoolEquals, AND-combined.** No OR (use multiple transitions), no `==`/`!=` on floats (fragile), no compound expressions. Covers the locomotion/jump/sprint cases. *(Easy to extend later.)*
3. **`kAnyState` ("*") transitions included** — cheap and high-value (e.g. → hit-react from anywhere). 
4. **Registration order = transition priority.** First satisfied transition wins. No explicit priority field.
5. **One float parameter bound per state for its blend space** (1D). 2D blend spaces don't exist yet (deferred from M51), so 2D binding is out of scope.
6. **No exit-time / "play once then auto-transition" conditions in v1** (e.g. jump-clip-finished → land). This needs `CharacterAnimator` to expose normalized playback completion, which it doesn't yet. **Deferred** — documented as the most likely follow-up. v1 transitions are purely parameter-driven.
7. **No nested/sub-state machines, no transition blend-interruption priority beyond registration order, no events/notifies.** Deferred.
8. **Continuous transition evaluation every frame** (Unity-style), relying on M51's mid-fade re-target for clean interruption. *(Alt: block transitions until the current fade completes — rejected as less responsive.)*
9. **Demo rewiring of `submitPlayerFox`** is included so there's something to visually gate later, but the core validation is the unit suite. The discrete footstep-state return and the M51 head look-at are preserved unchanged.

## Out of scope (explicitly)

Exit-time transitions, events/animation notifies, nested state machines, 2D blend binding, a serialized/asset-authored state graph (this is a code-built graph for now), and any editor UI. These are natural follow-ons once the runtime exists.
