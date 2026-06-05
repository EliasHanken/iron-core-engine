# M51 — Animation Blending + IK — Design

**Date:** 2026-06-05
**Status:** Approved (design), pending spec review
**Milestone:** M51 (first gameplay-feel milestone after the M50 render-polish/displacement track)

## Summary

Upgrade the engine's animation system from single-clip, hard-cut playback to **blended** animation (smooth cross-fade transitions + a 1D speed blend space) plus **inverse kinematics** (analytic two-bone IK + look-at). Demoed live on the net-shooter foxes, which already use the animation system. Built with parallel multi-agent execution — the core math units (TRS blend, two-bone IK, look-at) are independent and pure.

Scope decisions (confirmed with the user):
- **Blending:** cross-fade transitions + **1D blend space** (idle/walk/run by speed). Additive/layered blending deferred.
- **IK:** analytic **two-bone IK** + **look-at IK**. FABRIK (fox tail) deferred.
- **Demo:** the net-shooter foxes (cross-fade + speed blend + look-at clearly visible; two-bone IK demo firmed at build time).

Base: branched from `main` (`c0ef594`).

## What exists today (the foundation we extend)

- `engine/asset/Skeleton.h` — `Bone { int parentIndex; Mat4 inverseBindMatrix; Mat4 localBindTransform; string name; }`; `Skeleton { vector<Bone> bones; }` (flat, hierarchy via parentIndex).
- `engine/asset/Animation.h` — `AnimationClip` = `samplers` (keyframe streams, Linear/Step interp) + `channels` (each binds a sampler to a `(targetBone, path)` where path ∈ {Translation, Rotation, Scale}). `duration` precomputed.
- `engine/asset/AnimationPlayer.{h,cpp}` — drives ONE clip over a skeleton; `evaluate(span<Mat4> out)` samples channels → composes the hierarchy → writes the final skinning palette (`global · inverseBind`). Wraps at duration. **No way to get an intermediate pose** (this is what blocks blending).
- `engine/asset/CharacterAnimator.{h,cpp}` — state-name→clip map; `switchTo` is a **HARD CUT** (resets clip time, instant snap); `update(dt)` + `evaluate(palette)`.
- Net-shooter (`games/07-net-shooter`) loads `Fox.glb` (Survey/Walk/Run clips), renders players as skinned foxes via `CharacterAnimator` (hard-cut state switches today).
- Skinned rendering: `VkSkinnedMesh`, bones UBO at descriptor binding 9, `standardSkinnedLitVertSource` (M23). The renderer consumes a `span<Mat4>` bone palette — **unchanged by M51** (M51 only changes how that palette is computed).

## Architecture

The new evaluation pipeline (replaces "sample clip → palette"):
> sample clip(s) → local **`Pose`** → **blend** poses → compose global transforms → **IK** adjusts global bones → palette (`global · inverseBind`)

### A. Pose foundation (`engine/asset/Pose.h` + sampler refactor)

- **`BoneLocal { Vec3 translation; Quat rotation; Vec3 scale; }`** and **`Pose { std::vector<BoneLocal> bones; }`** — per-bone LOCAL transform (the thing you can blend before the hierarchy is applied). A `Pose`'s default for a bone is its `localBindTransform` decomposed to TRS.
- **`samplePose(const Skeleton&, const AnimationClip&, float time, Pose& out)`** — for each bone, start from the bind-pose TRS, then apply any T/R/S channels sampled at `time` (reuses the existing sampler interpolation logic — extract it from `AnimationPlayer.cpp` into a shared `sampleChannel` so the local sampling and the palette path share one implementation; DRY).
- **`posePalette(const Skeleton&, const Pose&, std::span<Mat4> out)`** — compose `local(TRS→Mat4)` up the hierarchy into global transforms, then write `global · inverseBind` per bone. (Optionally returns/exposes the global transforms for the IK pass — see C.)
- Needs a **`Quat`** with `slerp` + `toMat4` (and the inverse — Mat4 TRS decompose for bind poses). If the math lib lacks `Quat`/`slerp`, add it (`engine/math/Quat.h`) — pure + unit-tested. (The existing rotation channels already imply quaternion handling; confirm and reuse/extend.)
- `AnimationPlayer` is refactored onto this: it holds a `Pose`, `update` advances time, `evaluate` = `samplePose` → `posePalette`. Behavior for a single clip is unchanged (regression-safe).

### B. Blending (`engine/asset/PoseBlend.h` — pure, unit-tested)

- **`blendPose(const Pose& a, const Pose& b, float t, Pose& out)`** — per bone: `lerp` translation/scale, **`slerp`** rotation, by weight `t` ∈ [0,1]. `t=0`→a, `t=1`→b. (Assumes both poses share the skeleton's bone count.)
- **Cross-fade transitions** (in `CharacterAnimator`): `switchTo(state, fadeTime)` **keeps the previous clip advancing** during the fade (its time keeps running for natural motion — not frozen at switch instant) and blends previous-pose → new-clip-pose with weight ramping `0→1` over `fadeTime`. After the fade completes the previous clip is dropped. `fadeTime=0` reproduces the old hard cut. A `switchTo` issued mid-fade re-targets from the current blended pose (snapshot the in-progress pose as the new "previous") so rapid state changes don't pop.
- **1D blend space**: `BlendSpace1D { std::vector<std::pair<float, const AnimationClip*>> samples; }` (sorted by param). `sampleBlendSpace(skeleton, blendSpace, param, time, out)` finds the two bracketing `(param, clip)` entries, samples both at `time`, and `blendPose`s by the normalized weight (clamps to the ends). A state may be a single clip OR a blend space driven by a runtime param (e.g. speed).

### C. IK (`engine/asset/Ik.h` — pure, unit-tested; runs on global transforms)

IK operates AFTER blending, on the **global** bone transforms (before `· inverseBind`). So `posePalette` is split: compose global transforms → (IK pass mutates them) → write palette.
- **`solveTwoBoneIK(Vec3 root, Vec3 mid, Vec3 end, Vec3 target, Vec3 pole, ...)`** → returns the adjusted mid + end positions / the two bones' rotations. Analytic law-of-cosines: clamp target to total reach, compute the bend angle so the chain reaches the target, orient the plane via the pole/hint vector. Returns world-space rotations to write back into the two bones' global transforms (and propagate to children). For foot-plant / hand-reach.
- **`solveLookAt(Mat4 boneGlobal, Vec3 target, Vec3 forwardAxis, float maxAngleRad)`** → a rotation that points `forwardAxis` at `target`, clamped to `maxAngleRad` from the bone's rest forward. For head/aim tracking.
- **IK targets/chains** are configured on `CharacterAnimator` (world-space targets supplied by the game per frame). The IK pass: for each configured solver, read the relevant global bone transforms, solve, write back, recompute affected children's globals, then the palette.

### D. `CharacterAnimator` integration

Extend (keeping the existing API working — `switchTo(state)` = `switchTo(state, 0)` hard cut):
- `void switchTo(std::string_view state, float fadeTime);`
- `void setBlendSpaceForState(std::string state, BlendSpace1D space);`
- `void setBlendParam(float param);` (drives the active blend space, e.g. speed)
- `void addTwoBoneIK(int rootBone, int midBone, int endBone, ...);` + `void setIKTarget(handle, Vec3 worldTarget);` (+ a weight 0..1 to blend IK in/out)
- `void addLookAt(int bone, Vec3 forwardAxis, float maxAngle);` + `setLookAtTarget(handle, Vec3)`
- `evaluate(palette)` runs: sample (clip or blend space) → cross-fade blend (if transitioning) → compose globals → IK solvers → palette.

### E. Demo (net-shooter foxes, `games/07-net-shooter`)

- Replace the hard-cut `switchTo("run")` with `switchTo("run", 0.2f)` → **smooth cross-fades**.
- Define a **1D blend space** (idle/walk/run by the fox's speed) and feed `setBlendParam(speed)` so gait matches velocity.
- **Look-at:** add a head/neck look-at; per frame set the target to the fox's movement (or aim) direction → the head visibly tracks. (The clearest, most "alive" IK on flat ground.)
- **Two-bone IK:** demoed as a clear reach/plant — firmed at build time (e.g. a foot-plant target, or a reach toward the nearest player). The solver is general + unit-tested regardless.

## Data flow

```
game: switchTo(state, fade) / setBlendParam(speed) / setIKTarget(world) / setLookAtTarget(world)
  CharacterAnimator.update(dt): advance active (+ transitioning) clip times, ramp fade weight
  CharacterAnimator.evaluate(palette):
     poseA = sample(active state @ time)        // clip or blend-space(param)
     pose  = transitioning ? blendPose(posePrev, poseA, fadeW) : poseA
     globals = composeHierarchy(skeleton, pose)
     for each IK solver: solve(globals, target) -> write back -> fix children
     palette[i] = globals[i] * bone[i].inverseBind
  renderer.submitSkinned(..., palette)            // unchanged
```

## Error handling

- Null skeleton/clip → bind-pose palette (existing behavior preserved).
- Blend-space with a param outside the sample range → clamp to the nearest end clip.
- Two-bone IK target beyond reach → clamp to max reach (fully extended, no NaN); target at the root → degenerate-safe (return rest).
- Look-at target coincident with the bone → keep rest orientation (avoid NaN normalize).
- IK weight 0 → no change (pure passthrough); IK handles referencing out-of-range bones → ignored + logged once.
- Mismatched pose/skeleton bone counts → `blendPose`/`posePalette` operate over `min(count)` and log once.

## Testing

**Pure-math unit tests (TDD, headless — ideal for parallel agents):**
- `test_quat` (if Quat is added/extended): slerp endpoints + halfway + normalization + toMat4 round-trip.
- `test_pose_blend`: `blendPose` t=0/1/0.5 (lerp T/S, slerp R); blend-space param→bracketing+weight (ends clamp, exact-hit, midpoint).
- `test_two_bone_ik`: reachable target (end hits target within epsilon), unreachable (clamped to reach, fully extended), straight-line/degenerate, pole orientation.
- `test_look_at`: forward points at target; clamp at maxAngle; coincident-target safety.
- `test_character_animator`: cross-fade weight 0 = hard cut (matches pre-M51 single-clip palette); mid-fade blends; blend-space state evaluates.

**Visual gate (net-shooter):**
- Idle↔walk↔run **cross-fade** smoothly (no snap).
- Gait **blends with speed** (slow→walk, fast→run, in-between blends).
- Fox **head tracks** its movement/aim (look-at).
- Two-bone IK reach/plant reads correctly; no popping/jitter; nothing NaNs.

## Files (anticipated)

- `engine/math/Quat.h` (+ test) — quaternion + slerp + toMat4, if missing/incomplete.
- `engine/asset/Pose.h` (+ `.cpp`) — `Pose`/`BoneLocal`, `samplePose`, `composeGlobals`, `posePalette`.
- `engine/asset/PoseBlend.h` (+ test) — `blendPose`, `BlendSpace1D`, `sampleBlendSpace`.
- `engine/asset/Ik.h` (+ `.cpp` + test) — `solveTwoBoneIK`, `solveLookAt`.
- `engine/asset/AnimationPlayer.{h,cpp}` — refactor onto Pose (regression-safe).
- `engine/asset/CharacterAnimator.{h,cpp}` — cross-fade + blend space + IK config (+ test).
- `engine/asset/GltfLoader.cpp` — extract `sampleChannel` if needed (DRY between palette + pose paths).
- `games/07-net-shooter/main.cpp` — demo: cross-fades + speed blend space + look-at (+ two-bone IK).

## Out of scope (later milestones)

- **Additive / layered blending** (upper-body aim over lower-body locomotion; bone masks).
- **FABRIK** multi-bone chains (the fox tail).
- **2D blend spaces** (directional locomotion), root motion, animation retargeting, morph-target (weights) animation.
