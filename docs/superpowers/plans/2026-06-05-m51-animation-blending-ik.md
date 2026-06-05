# M51 — Animation Blending + IK — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the animation system from single-clip hard-cut playback to blended animation (cross-fade + 1D speed blend space) plus inverse kinematics (analytic two-bone + look-at), demoed on the net-shooter foxes.

**Architecture:** Insert a local-TRS `Pose` layer between clip sampling and the skinning palette. `samplePose` → (`blendPose` / `sampleBlendSpace`) → `composeGlobals` → IK adjusts globals → `globalsToPalette`. `AnimationPlayer` and `CharacterAnimator` are refactored onto this layer; the renderer's `span<Mat4>` palette interface is unchanged. The two-bone and look-at solvers are pure functions on world-space positions/rotations.

**Tech Stack:** C++17, existing `iron::` math (`Vec3`, `Mat4`, `Quat` with `slerp`/`toMat4`/`fromAxisAngle`), the `test_framework.h` CHECK/CHECK_NEAR harness, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-06-05-m51-animation-blending-ik-design.md`

**Conventions for every commit in this plan:** end the commit message body with
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## File Structure

**New engine files:**
- `engine/asset/Pose.h` / `engine/asset/Pose.cpp` — `BoneLocal`/`Pose`, TRS compose/decompose, `bindPose`, `samplePose`, `composeGlobals`, `globalsToPalette`, `posePalette`. Owns the sampler math extracted from `AnimationPlayer.cpp`.
- `engine/asset/PoseBlend.h` / `engine/asset/PoseBlend.cpp` — `blendPose`, `BlendSpace1D`, `sampleBlendSpace`.
- `engine/asset/Ik.h` / `engine/asset/Ik.cpp` — `rotationFromTo`, `solveTwoBoneIK`, `solveLookAt`.

**Modified engine files:**
- `engine/asset/AnimationPlayer.cpp` — refactor `evaluate` onto `Pose` (remove the now-shared helpers).
- `engine/asset/CharacterAnimator.h` / `.cpp` — cross-fade, blend space, IK config + application (own time + pose pipeline).
- `engine/CMakeLists.txt` — register the three new `.cpp` files.

**New tests:**
- `tests/test_pose.cpp`, `tests/test_pose_blend.cpp`, `tests/test_two_bone_ik.cpp`, `tests/test_look_at.cpp` — plus extensions to the existing `tests/test_character_animator.cpp`. `tests/CMakeLists.txt` registers the new ones.

**Demo:**
- `games/07-net-shooter/main.cpp` — cross-fades, speed blend space, head look-at.

**Note on `Quat`:** `engine/math/Quaternion.h` already provides `Quat` with `slerp`, `toMat4`, `fromAxisAngle`, `normalized`, `rotate`, and the free `slerp`/`dot`. No new quaternion type is needed (the spec's "add `engine/math/Quat.h` if missing" resolves to "already present — reuse").

**Phase-sync caveat (out of scope, document in code):** blend-space clips of differing duration share one `time` wrapped by the active clip's duration; cross-clip phase matching (foot-sync) is a later refinement.

---

## Task 1: Pose foundation — types, TRS compose/decompose, bind pose

**Files:**
- Create: `engine/asset/Pose.h`
- Create: `engine/asset/Pose.cpp`
- Modify: `engine/CMakeLists.txt` (add `asset/Pose.cpp`)
- Modify: `tests/CMakeLists.txt` (register `test_pose`)
- Test: `tests/test_pose.cpp`

- [ ] **Step 1: Create the header `engine/asset/Pose.h`**

```cpp
#pragma once

#include "asset/Animation.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <span>
#include <vector>

namespace iron {

// One bone's LOCAL transform, decomposed so it can be blended before the
// hierarchy is applied. Defaults are the identity transform.
struct BoneLocal {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// A full local-space pose: one BoneLocal per skeleton bone, in bone order.
struct Pose {
    std::vector<BoneLocal> bones;
};

// Column-major TRS compose (M = T * R * S, no shear).
Mat4 composeTRS(Vec3 t, Quat r, Vec3 s);
Mat4 composeTRS(const BoneLocal& b);

// Decompose a column-major TRS Mat4 (no shear) into T/R/S. Scale comes from
// basis-column lengths; rotation from the normalized basis as a quaternion.
BoneLocal decomposeTRS(const Mat4& m);

// Fill `out` with the skeleton's bind pose (each localBindTransform decomposed).
void bindPose(const Skeleton& skeleton, Pose& out);

// Sample a clip at `time` into a local Pose: seed from the bind pose, then let
// each channel overwrite the component it drives. `out` is resized to the
// skeleton's bone count.
void samplePose(const Skeleton& skeleton, const AnimationClip& clip,
                float time, Pose& out);

// Compose a local Pose up the hierarchy into global bone transforms. Writes
// min(out, pose, skeleton) bones; parents must precede children (loader
// guarantees this).
void composeGlobals(const Skeleton& skeleton, const Pose& pose,
                    std::span<Mat4> outGlobals);

// Write the skinning palette (global * inverseBind) from precomputed globals.
void globalsToPalette(const Skeleton& skeleton, std::span<const Mat4> globals,
                      std::span<Mat4> out);

// Convenience: composeGlobals + globalsToPalette (no IK pass).
void posePalette(const Skeleton& skeleton, const Pose& pose,
                 std::span<Mat4> out);

}  // namespace iron
```

- [ ] **Step 2: Write the failing test `tests/test_pose.cpp` (compose/decompose + bindPose only for now)**

```cpp
#include "asset/Pose.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <array>

using namespace iron;

int main() {
    // decompose(compose(t,r,s)) round-trips.
    {
        const Vec3 t{3.0f, -2.0f, 5.0f};
        const Quat r = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.0f);
        const Vec3 s{2.0f, 2.0f, 2.0f};
        const BoneLocal b = decomposeTRS(composeTRS(t, r, s));
        CHECK_NEAR(b.translation.x, 3.0f);
        CHECK_NEAR(b.translation.y, -2.0f);
        CHECK_NEAR(b.translation.z, 5.0f);
        CHECK_NEAR(b.scale.x, 2.0f);
        CHECK_NEAR(b.scale.y, 2.0f);
        CHECK_NEAR(b.scale.z, 2.0f);
        // Rotation sign can flip (q == -q); compare via the rotated basis.
        const Vec3 v = b.rotation.rotate(Vec3{1, 0, 0});
        const Vec3 w = r.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, w.x);
        CHECK_NEAR(v.y, w.y);
        CHECK_NEAR(v.z, w.z);
    }

    // bindPose decomposes each bone's localBindTransform.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = composeTRS(Vec3{1, 2, 3}, Quat::identity(),
                                             Vec3{1, 1, 1});
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        Pose p;
        bindPose(sk, p);
        CHECK(p.bones.size() == 1);
        CHECK_NEAR(p.bones[0].translation.x, 1.0f);
        CHECK_NEAR(p.bones[0].translation.y, 2.0f);
        CHECK_NEAR(p.bones[0].translation.z, 3.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Run the test to verify it fails to build/link**

Run: `cmake --build build --target test_pose` (after Step 5 registers it)
Expected: FAIL — `Pose.cpp` functions undefined (or target not yet registered).

- [ ] **Step 4: Implement `engine/asset/Pose.cpp` (this task: compose/decompose + bindPose; sampler functions added in Task 2)**

Move `composeTRS`/`decomposeTRS` out of `AnimationPlayer.cpp` to here (as public `iron::` functions). Implement `bindPose`. Leave `samplePose`/`composeGlobals`/`globalsToPalette`/`posePalette` as defined in Task 2 — but to keep this task self-contained and compiling, implement ALL of them now using the bodies below (Task 2 only adds their tests).

```cpp
#include "asset/Pose.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace iron {

namespace {

// Returns index i with inputs[i] <= t < inputs[i+1], clamped to the ends;
// (size_t)-1 for an empty inputs vector (callers handle).
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
    const Vec3 v1{s.outputs[(i + 1) * 3 + 0], s.outputs[(i + 1) * 3 + 1],
                  s.outputs[(i + 1) * 3 + 2]};
    const float a = (t - s.inputs[i]) / (s.inputs[i + 1] - s.inputs[i]);
    return interpolate(v0, v1, a);
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
    const float a = (t - s.inputs[i]) / (s.inputs[i + 1] - s.inputs[i]);
    return Quat::slerp(q0, q1, a);
}

}  // namespace

Mat4 composeTRS(Vec3 t, Quat r, Vec3 s) {
    Mat4 m = r.toMat4();
    m.at(0, 0) *= s.x; m.at(1, 0) *= s.x; m.at(2, 0) *= s.x;
    m.at(0, 1) *= s.y; m.at(1, 1) *= s.y; m.at(2, 1) *= s.y;
    m.at(0, 2) *= s.z; m.at(1, 2) *= s.z; m.at(2, 2) *= s.z;
    m.at(0, 3) = t.x; m.at(1, 3) = t.y; m.at(2, 3) = t.z;
    m.at(3, 3) = 1.0f;
    return m;
}

Mat4 composeTRS(const BoneLocal& b) {
    return composeTRS(b.translation, b.rotation, b.scale);
}

BoneLocal decomposeTRS(const Mat4& m) {
    BoneLocal out;
    out.translation = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    const Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    out.scale = Vec3{length(c0), length(c1), length(c2)};

    Mat4 rot = Mat4::identity();
    if (out.scale.x > 1e-8f) { rot.at(0, 0) = c0.x / out.scale.x; rot.at(1, 0) = c0.y / out.scale.x; rot.at(2, 0) = c0.z / out.scale.x; }
    if (out.scale.y > 1e-8f) { rot.at(0, 1) = c1.x / out.scale.y; rot.at(1, 1) = c1.y / out.scale.y; rot.at(2, 1) = c1.z / out.scale.y; }
    if (out.scale.z > 1e-8f) { rot.at(0, 2) = c2.x / out.scale.z; rot.at(1, 2) = c2.y / out.scale.z; rot.at(2, 2) = c2.z / out.scale.z; }

    Quat& r = out.rotation;
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
    return out;
}

void bindPose(const Skeleton& skeleton, Pose& out) {
    out.bones.resize(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        out.bones[i] = decomposeTRS(skeleton.bones[i].localBindTransform);
    }
}

void samplePose(const Skeleton& skeleton, const AnimationClip& clip,
                float time, Pose& out) {
    const std::size_t n = skeleton.bones.size();
    bindPose(skeleton, out);  // seed every bone from bind pose
    for (const auto& ch : clip.channels) {
        if (ch.targetBone < 0 || static_cast<std::size_t>(ch.targetBone) >= n) continue;
        if (ch.samplerIndex < 0 ||
            static_cast<std::size_t>(ch.samplerIndex) >= clip.samplers.size()) continue;
        const AnimationSampler& s = clip.samplers[ch.samplerIndex];
        switch (ch.path) {
            case AnimationPath::Translation:
                out.bones[ch.targetBone].translation = sampleVec3(s, time);
                break;
            case AnimationPath::Rotation:
                out.bones[ch.targetBone].rotation = sampleQuat(s, time);
                break;
            case AnimationPath::Scale:
                out.bones[ch.targetBone].scale = sampleVec3(s, time);
                break;
        }
    }
}

void composeGlobals(const Skeleton& skeleton, const Pose& pose,
                    std::span<Mat4> outGlobals) {
    const std::size_t n = std::min({outGlobals.size(), pose.bones.size(),
                                    skeleton.bones.size()});
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4 local = composeTRS(pose.bones[i]);
        const int parent = skeleton.bones[i].parentIndex;
        if (parent < 0) {
            outGlobals[i] = local;
        } else {
            assert(static_cast<std::size_t>(parent) < i &&
                   "bone parent must have lower index");
            outGlobals[i] = outGlobals[parent] * local;
        }
    }
}

void globalsToPalette(const Skeleton& skeleton, std::span<const Mat4> globals,
                      std::span<Mat4> out) {
    const std::size_t n = std::min({globals.size(), out.size(),
                                    skeleton.bones.size()});
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = globals[i] * skeleton.bones[i].inverseBindMatrix;
    }
}

void posePalette(const Skeleton& skeleton, const Pose& pose,
                 std::span<Mat4> out) {
    const std::size_t n = std::min({out.size(), pose.bones.size(),
                                    skeleton.bones.size()});
    std::vector<Mat4> globals(n);
    composeGlobals(skeleton, pose, globals);
    globalsToPalette(skeleton, globals, out);
}

}  // namespace iron
```

- [ ] **Step 5: Register the source + test in CMake**

In `engine/CMakeLists.txt`, add `asset/Pose.cpp` immediately after the `asset/AnimationPlayer.cpp` line:
```cmake
  asset/AnimationPlayer.cpp
  asset/Pose.cpp
  asset/CharacterAnimator.cpp
```

In `tests/CMakeLists.txt`, add after the `test_character_animator` line:
```cmake
iron_add_test(test_pose test_pose.cpp)
```

- [ ] **Step 6: Build and run the test**

Run: `cmake --build build --target test_pose && ctest --test-dir build -R test_pose --output-on-failure`
Expected: PASS — "OK - all checks passed".

- [ ] **Step 7: Commit**

```bash
git add engine/asset/Pose.h engine/asset/Pose.cpp engine/CMakeLists.txt tests/test_pose.cpp tests/CMakeLists.txt
git commit -m "M51: Pose foundation (local-TRS pose, compose/decompose, sampling, palette)"
```

---

## Task 2: Pose sampling + palette tests

The functions already exist (Task 1 implemented all of `Pose.cpp`). This task adds the sampling/palette coverage that locks their behavior.

**Files:**
- Modify: `tests/test_pose.cpp`

- [ ] **Step 1: Add failing tests for samplePose + posePalette**

Insert before `return iron_test_result();` in `tests/test_pose.cpp`:

```cpp
    // samplePose drives a bone's translation from a clip, then posePalette
    // produces global * inverseBind (identity inverseBind => global).
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        AnimationClip clip;
        clip.name = "slide";
        clip.duration = 1.0f;
        AnimationSampler s;
        s.inputs = {0.0f, 1.0f};
        s.outputs = {0, 0, 0,  10, 0, 0};
        s.interpolation = AnimationInterpolation::Linear;
        clip.samplers.push_back(s);
        AnimationChannel ch;
        ch.targetBone = 0;
        ch.path = AnimationPath::Translation;
        ch.samplerIndex = 0;
        clip.channels.push_back(ch);

        Pose pose;
        samplePose(sk, clip, 0.5f, pose);
        CHECK_NEAR(pose.bones[0].translation.x, 5.0f);

        std::array<Mat4, 1> palette{};
        posePalette(sk, pose, palette);
        CHECK_NEAR(palette[0].at(0, 3), 5.0f);
    }

    // composeGlobals concatenates a parent translation into a child.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = composeTRS(Vec3{10, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
        root.inverseBindMatrix = Mat4::identity();
        Bone child;
        child.parentIndex = 0;
        child.localBindTransform = composeTRS(Vec3{1, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
        child.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);
        sk.bones.push_back(child);

        Pose pose;
        bindPose(sk, pose);
        std::array<Mat4, 2> globals{};
        composeGlobals(sk, pose, globals);
        CHECK_NEAR(globals[0].at(0, 3), 10.0f);
        CHECK_NEAR(globals[1].at(0, 3), 11.0f);  // 10 (parent) + 1 (child)
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build --target test_pose && ctest --test-dir build -R test_pose --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/test_pose.cpp
git commit -m "M51: cover samplePose, composeGlobals, posePalette"
```

---

## Task 3: Refactor AnimationPlayer onto Pose

**Files:**
- Modify: `engine/asset/AnimationPlayer.cpp`
- Test: `tests/test_animation_player.cpp` (existing — must still pass, no edits)

- [ ] **Step 1: Replace `AnimationPlayer.cpp` body with the Pose-based implementation**

Replace the entire file contents with:

```cpp
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
```

This removes the duplicated `composeTRS`/`decomposeTRS`/`findKeyframe`/`sampleVec3`/`sampleQuat` (now shared in `Pose.cpp`). Behavior is identical: no skeleton → no-op; skeleton, no clip → bind pose; clip → sampled palette.

- [ ] **Step 2: Build and run the existing AnimationPlayer test (regression gate)**

Run: `cmake --build build --target test_animation_player && ctest --test-dir build -R test_animation_player --output-on-failure`
Expected: PASS — all pre-existing checks (bind pose, 90° Y at t=duration, step interp, empty-sampler alignment, wrap) still pass.

- [ ] **Step 3: Commit**

```bash
git add engine/asset/AnimationPlayer.cpp
git commit -m "M51: refactor AnimationPlayer onto shared Pose layer (no behavior change)"
```

---

## Task 4: blendPose (lerp T/S, slerp R)

**Files:**
- Create: `engine/asset/PoseBlend.h`
- Create: `engine/asset/PoseBlend.cpp`
- Modify: `engine/CMakeLists.txt` (add `asset/PoseBlend.cpp`)
- Modify: `tests/CMakeLists.txt` (register `test_pose_blend`)
- Test: `tests/test_pose_blend.cpp`

- [ ] **Step 1: Create `engine/asset/PoseBlend.h` (blendPose only this task; BlendSpace1D added in Task 5)**

```cpp
#pragma once

#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/Skeleton.h"

#include <utility>
#include <vector>

namespace iron {

// Per-bone blend: lerp translation/scale, slerp rotation, by weight t in
// [0,1] (t=0 -> a, t=1 -> b; t is clamped). Operates over min(a,b) bones;
// `out` is resized to that count.
void blendPose(const Pose& a, const Pose& b, float t, Pose& out);

}  // namespace iron
```

- [ ] **Step 2: Write the failing test `tests/test_pose_blend.cpp`**

```cpp
#include "asset/Pose.h"
#include "asset/PoseBlend.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

namespace {
Pose makePose(Vec3 t, Quat r, Vec3 s) {
    Pose p;
    p.bones.push_back(BoneLocal{t, r, s});
    return p;
}
}  // namespace

int main() {
    const Pose a = makePose(Vec3{0, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
    const Pose b = makePose(Vec3{10, 0, 0},
                            Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5707963f),
                            Vec3{2, 2, 2});

    // t=0 -> a
    {
        Pose out;
        blendPose(a, b, 0.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 0.0f);
        CHECK_NEAR(out.bones[0].scale.x, 1.0f);
    }
    // t=1 -> b
    {
        Pose out;
        blendPose(a, b, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
        CHECK_NEAR(out.bones[0].scale.x, 2.0f);
    }
    // t=0.5 -> midpoint T/S, slerp R (45 deg about +Y)
    {
        Pose out;
        blendPose(a, b, 0.5f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
        CHECK_NEAR(out.bones[0].scale.x, 1.5f);
        // 45 deg about +Y rotates +X toward -Z by 45 deg: x=cos45, z=-sin45.
        const Vec3 v = out.bones[0].rotation.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.70710678f);
        CHECK_NEAR(v.z, -0.70710678f);
    }
    // t clamps below 0 and above 1.
    {
        Pose lo, hi;
        blendPose(a, b, -1.0f, lo);
        blendPose(a, b, 2.0f, hi);
        CHECK_NEAR(lo.bones[0].translation.x, 0.0f);
        CHECK_NEAR(hi.bones[0].translation.x, 10.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_pose_blend` (after Step 5 registers it)
Expected: FAIL — `blendPose` undefined.

- [ ] **Step 4: Implement `engine/asset/PoseBlend.cpp`**

```cpp
#include "asset/PoseBlend.h"

#include <algorithm>

namespace iron {

void blendPose(const Pose& a, const Pose& b, float t, Pose& out) {
    t = std::clamp(t, 0.0f, 1.0f);
    const std::size_t n = std::min(a.bones.size(), b.bones.size());
    out.bones.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.bones[i].translation = interpolate(a.bones[i].translation,
                                               b.bones[i].translation, t);
        out.bones[i].scale = interpolate(a.bones[i].scale, b.bones[i].scale, t);
        out.bones[i].rotation = slerp(a.bones[i].rotation, b.bones[i].rotation, t);
    }
}

}  // namespace iron
```

- [ ] **Step 5: Register source + test**

`engine/CMakeLists.txt` — after `asset/Pose.cpp`:
```cmake
  asset/Pose.cpp
  asset/PoseBlend.cpp
```
`tests/CMakeLists.txt` — after the `test_pose` line:
```cmake
iron_add_test(test_pose_blend test_pose_blend.cpp)
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build --target test_pose_blend && ctest --test-dir build -R test_pose_blend --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/PoseBlend.h engine/asset/PoseBlend.cpp engine/CMakeLists.txt tests/test_pose_blend.cpp tests/CMakeLists.txt
git commit -m "M51: blendPose (lerp T/S, slerp R)"
```

---

## Task 5: BlendSpace1D + sampleBlendSpace

**Files:**
- Modify: `engine/asset/PoseBlend.h`
- Modify: `engine/asset/PoseBlend.cpp`
- Modify: `tests/test_pose_blend.cpp`

- [ ] **Step 1: Add `BlendSpace1D` + `sampleBlendSpace` to `PoseBlend.h`**

Insert before the closing `}  // namespace iron`:

```cpp
// A 1D blend space: clips placed along a scalar axis (e.g. speed). Samples are
// kept sorted ascending by param via add().
struct BlendSpace1D {
    std::vector<std::pair<float, const AnimationClip*>> samples;

    // Insert (param, clip) keeping `samples` sorted ascending by param.
    void add(float param, const AnimationClip* clip);
};

// Sample the blend space at `param` and `time`: find the two bracketing
// samples, sample both clips at `time`, and blendPose by the normalized
// weight. Clamps to the end clips outside the param range. With a single
// sample, returns that clip's pose. With no samples, returns the bind pose.
void sampleBlendSpace(const Skeleton& skeleton, const BlendSpace1D& space,
                      float param, float time, Pose& out);
```

- [ ] **Step 2: Add failing tests to `tests/test_pose_blend.cpp`**

Add these includes at the top (alongside existing ones):
```cpp
#include "asset/Animation.h"
#include "asset/Skeleton.h"
```
Add a helper in the anonymous namespace:
```cpp
Skeleton oneBoneSkeleton() {
    Skeleton sk;
    Bone b;
    b.parentIndex = -1;
    b.localBindTransform = Mat4::identity();
    b.inverseBindMatrix = Mat4::identity();
    sk.bones.push_back(b);
    return sk;
}
AnimationClip slideClip(float endX) {
    AnimationClip c;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs = {0.0f, 1.0f};
    s.outputs = {0, 0, 0, endX, 0, 0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch;
    ch.targetBone = 0;
    ch.path = AnimationPath::Translation;
    ch.samplerIndex = 0;
    c.channels.push_back(ch);
    return c;
}
```
Add `#include "math/Mat4.h"` if not present. Then before `return iron_test_result();`:
```cpp
    {
        const Skeleton sk = oneBoneSkeleton();
        const AnimationClip walk = slideClip(0.0f);   // at param 0, bone x stays 0
        const AnimationClip run  = slideClip(10.0f);  // at param 1, bone x -> 10 at t=1

        BlendSpace1D space;
        space.add(1.0f, &run);   // add out of order on purpose
        space.add(0.0f, &walk);
        CHECK(space.samples.size() == 2);
        CHECK_NEAR(space.samples[0].first, 0.0f);  // sorted ascending

        Pose out;
        // midpoint param at t=1: blend of x=0 and x=10 -> x=5
        sampleBlendSpace(sk, space, 0.5f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 5.0f);
        // param below range clamps to walk (x=0)
        sampleBlendSpace(sk, space, -3.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 0.0f);
        // param above range clamps to run (x=10)
        sampleBlendSpace(sk, space, 5.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
        // exact hit on run param
        sampleBlendSpace(sk, space, 1.0f, 1.0f, out);
        CHECK_NEAR(out.bones[0].translation.x, 10.0f);
    }
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_pose_blend`
Expected: FAIL — `BlendSpace1D::add` / `sampleBlendSpace` undefined.

- [ ] **Step 4: Implement in `PoseBlend.cpp`**

Add `#include <cstddef>` and implement at the end of the namespace:

```cpp
void BlendSpace1D::add(float param, const AnimationClip* clip) {
    auto it = samples.begin();
    while (it != samples.end() && it->first < param) ++it;
    samples.insert(it, std::make_pair(param, clip));
}

void sampleBlendSpace(const Skeleton& skeleton, const BlendSpace1D& space,
                      float param, float time, Pose& out) {
    if (space.samples.empty()) {
        bindPose(skeleton, out);
        return;
    }
    if (space.samples.size() == 1 || param <= space.samples.front().first) {
        samplePose(skeleton, *space.samples.front().second, time, out);
        return;
    }
    if (param >= space.samples.back().first) {
        samplePose(skeleton, *space.samples.back().second, time, out);
        return;
    }
    // Find the bracket [lo, hi] with samples[lo].param <= param < samples[hi].
    std::size_t hi = 1;
    while (hi < space.samples.size() && space.samples[hi].first <= param) ++hi;
    const std::size_t lo = hi - 1;
    const float p0 = space.samples[lo].first;
    const float p1 = space.samples[hi].first;
    const float w = (p1 > p0) ? (param - p0) / (p1 - p0) : 0.0f;

    Pose a, b;
    samplePose(skeleton, *space.samples[lo].second, time, a);
    samplePose(skeleton, *space.samples[hi].second, time, b);
    blendPose(a, b, w, out);
}
```

Guard against null clips defensively: if any bracketing `second` is null, fall back to `bindPose(skeleton, out)`. Add at the top of `sampleBlendSpace` after the empty check is fine, but to keep it concrete, prepend inside the bracket branch:
```cpp
    if (!space.samples[lo].second || !space.samples[hi].second) {
        bindPose(skeleton, out);
        return;
    }
```
(Place this immediately before the two `samplePose` calls.)

- [ ] **Step 5: Build and run**

Run: `cmake --build build --target test_pose_blend && ctest --test-dir build -R test_pose_blend --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/asset/PoseBlend.h engine/asset/PoseBlend.cpp tests/test_pose_blend.cpp
git commit -m "M51: BlendSpace1D + sampleBlendSpace (bracketed 1D blend)"
```

---

## Task 6: solveTwoBoneIK (analytic two-bone)

**Files:**
- Create: `engine/asset/Ik.h`
- Create: `engine/asset/Ik.cpp`
- Modify: `engine/CMakeLists.txt` (add `asset/Ik.cpp`)
- Modify: `tests/CMakeLists.txt` (register `test_two_bone_ik`)
- Test: `tests/test_two_bone_ik.cpp`

- [ ] **Step 1: Create `engine/asset/Ik.h`**

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"

namespace iron {

// Result of a two-bone solve. Positions are world-space; deltas are world-space
// rotations to apply on top of the root/mid bones' current global rotations.
struct TwoBoneIKResult {
    Vec3 midPos;     // new world position of the mid joint (knee/elbow)
    Vec3 endPos;     // new world position of the end joint (~target if reachable)
    Quat rootDelta;  // world-space rotation delta for the root bone
    Quat midDelta;   // world-space rotation delta for the mid bone
};

// Shortest-arc rotation taking `from` onto `to` (inputs need not be unit).
// Identity for parallel inputs; a 180-degree rotation about an arbitrary
// perpendicular for anti-parallel inputs.
Quat rotationFromTo(Vec3 from, Vec3 to);

// Analytic two-bone IK. `root`/`mid`/`end` are the current world joint
// positions; `target` is where the end should reach; `pole` hints the bend
// direction (the mid joint bends toward the side of `pole`). The target is
// clamped to the reachable range so the chain never tears or NaNs.
TwoBoneIKResult solveTwoBoneIK(Vec3 root, Vec3 mid, Vec3 end,
                               Vec3 target, Vec3 pole);

}  // namespace iron
```

- [ ] **Step 2: Write the failing test `tests/test_two_bone_ik.cpp`**

```cpp
#include "asset/Ik.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

int main() {
    // Reachable target: end reaches target; segment lengths preserved; mid
    // bends toward +Y (pole). Chain root(0,0,0)-mid(1,0,0)-end(2,0,0),
    // lengths 1 and 1. Target (1,1,0) is reachable (max reach ~2).
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const Vec3 target{1, 1, 0}, pole{0, 1, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, target, pole);
        CHECK_NEAR(r.endPos.x, 1.0f);
        CHECK_NEAR(r.endPos.y, 1.0f);
        CHECK_NEAR(r.endPos.z, 0.0f);
        CHECK_NEAR(length(r.midPos - root), 1.0f);          // lab preserved
        CHECK_NEAR(length(r.endPos - r.midPos), 1.0f);       // lcb preserved
        CHECK(r.midPos.y > 0.0f);                            // bent toward pole
    }

    // Unreachable target: chain fully extends toward target, no NaN.
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const Vec3 target{5, 0, 0}, pole{0, 1, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, target, pole);
        CHECK_NEAR(r.endPos.x, 2.0f);                        // ~ lab+lcb
        CHECK(std::isfinite(r.endPos.x));
        CHECK(std::isfinite(r.midPos.y));
        CHECK_NEAR(length(r.midPos - root), 1.0f);
    }

    // Degenerate: target at the root. Must not NaN.
    {
        const Vec3 root{0, 0, 0}, mid{1, 0, 0}, end{2, 0, 0};
        const TwoBoneIKResult r = solveTwoBoneIK(root, mid, end, root, Vec3{0, 1, 0});
        CHECK(std::isfinite(r.midPos.x));
        CHECK(std::isfinite(r.endPos.x));
    }

    // rotationFromTo basic: +X onto +Y.
    {
        const Quat q = rotationFromTo(Vec3{1, 0, 0}, Vec3{0, 1, 0});
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.0f);
        CHECK_NEAR(v.y, 1.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_two_bone_ik` (after Step 5 registers it)
Expected: FAIL — `solveTwoBoneIK` undefined.

- [ ] **Step 4: Implement `engine/asset/Ik.cpp`**

```cpp
#include "asset/Ik.h"

#include <algorithm>
#include <cmath>

namespace iron {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

Quat rotationFromTo(Vec3 from, Vec3 to) {
    from = normalize(from);
    to = normalize(to);
    const float d = dot(from, to);
    if (d >= 1.0f - 1e-6f) return Quat::identity();
    if (d <= -1.0f + 1e-6f) {
        Vec3 axis = cross(Vec3{1, 0, 0}, from);
        if (length(axis) < 1e-4f) axis = cross(Vec3{0, 1, 0}, from);
        return Quat::fromAxisAngle(normalize(axis), kPi);
    }
    const Vec3 axis = normalize(cross(from, to));
    return Quat::fromAxisAngle(axis, std::acos(d));
}

TwoBoneIKResult solveTwoBoneIK(Vec3 root, Vec3 mid, Vec3 end,
                               Vec3 target, Vec3 pole) {
    const float eps = 1e-5f;
    const float lab = length(mid - root);  // root -> mid bone length
    const float lcb = length(end - mid);   // mid -> end bone length

    // Direction to target and clamped reach.
    const Vec3 toTarget = target - root;
    float lat = length(toTarget);
    const Vec3 dirAT = (lat > eps) ? toTarget * (1.0f / lat) : Vec3{1, 0, 0};
    const float minReach = std::fabs(lab - lcb) + eps;
    const float maxReach = lab + lcb - eps;
    lat = std::clamp(lat, minReach, maxReach);

    // New end position (== target when reachable).
    const Vec3 endPos = root + dirAT * lat;

    // Angle at the root between (root->mid) and (root->end), law of cosines.
    float cosRoot = (lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat);
    cosRoot = std::clamp(cosRoot, -1.0f, 1.0f);
    const float rootAngle = std::acos(cosRoot);

    // Bend direction: component of (pole-root) perpendicular to dirAT.
    Vec3 bend = (pole - root) - dirAT * dot(pole - root, dirAT);
    if (length(bend) < eps) {
        bend = cross(dirAT, Vec3{0, 1, 0});
        if (length(bend) < eps) bend = cross(dirAT, Vec3{1, 0, 0});
    }
    bend = normalize(bend);

    // Mid position in the (dirAT, bend) plane.
    const Vec3 midPos = root
        + dirAT * (lab * std::cos(rootAngle))
        + bend * (lab * std::sin(rootAngle));

    // World-space rotation deltas so the bones orient along the new segments.
    const Quat rootDelta = rotationFromTo(mid - root, midPos - root);
    const Vec3 carriedMidEnd = rootDelta.rotate(end - mid);
    const Quat midDelta = rotationFromTo(carriedMidEnd, endPos - midPos);

    return TwoBoneIKResult{midPos, endPos, rootDelta, midDelta};
}

}  // namespace iron
```

- [ ] **Step 5: Register source + test**

`engine/CMakeLists.txt` — after `asset/PoseBlend.cpp`:
```cmake
  asset/PoseBlend.cpp
  asset/Ik.cpp
```
`tests/CMakeLists.txt` — after `test_pose_blend`:
```cmake
iron_add_test(test_two_bone_ik test_two_bone_ik.cpp)
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build --target test_two_bone_ik && ctest --test-dir build -R test_two_bone_ik --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/Ik.h engine/asset/Ik.cpp engine/CMakeLists.txt tests/test_two_bone_ik.cpp tests/CMakeLists.txt
git commit -m "M51: analytic two-bone IK (reach-clamped, pole-oriented)"
```

---

## Task 7: solveLookAt (clamped aim)

**Files:**
- Modify: `engine/asset/Ik.h`
- Modify: `engine/asset/Ik.cpp`
- Modify: `tests/CMakeLists.txt` (register `test_look_at`)
- Test: `tests/test_look_at.cpp`

- [ ] **Step 1: Add `solveLookAt` to `Ik.h`**

Insert before the closing `}  // namespace iron`:

```cpp
// Look-at: a world-space rotation that turns `currentForward` (the bone's
// current world forward direction) toward `target` as seen from `bonePos`,
// limited to at most `maxAngleRad`. Identity when degenerate (coincident
// target or zero forward).
Quat solveLookAt(Vec3 bonePos, Vec3 currentForward, Vec3 target,
                 float maxAngleRad);
```

- [ ] **Step 2: Write the failing test `tests/test_look_at.cpp`**

```cpp
#include "asset/Ik.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

int main() {
    // Unclamped: forward +X turned toward target at -Z (90 deg) lands on -Z.
    {
        const Quat q = solveLookAt(Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                   Vec3{0, 0, -1}, 3.14159f);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 0.0f);
        CHECK_NEAR(v.z, -1.0f);
    }
    // Clamped: same geometry but limited to 0.2 rad -> result turns exactly
    // 0.2 rad (angle between forward and rotated-forward == 0.2).
    {
        const float maxA = 0.2f;
        const Quat q = solveLookAt(Vec3{0, 0, 0}, Vec3{1, 0, 0},
                                   Vec3{0, 0, -1}, maxA);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        const float ang = std::acos(std::clamp(dot(normalize(v), Vec3{1, 0, 0}),
                                               -1.0f, 1.0f));
        CHECK_NEAR(ang, maxA);
    }
    // Coincident target -> identity (forward unchanged).
    {
        const Quat q = solveLookAt(Vec3{2, 2, 2}, Vec3{1, 0, 0},
                                   Vec3{2, 2, 2}, 1.0f);
        const Vec3 v = q.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, 1.0f);
        CHECK_NEAR(v.y, 0.0f);
        CHECK_NEAR(v.z, 0.0f);
    }

    return iron_test_result();
}
```
Add `#include <algorithm>` for `std::clamp` in the test.

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_look_at`
Expected: FAIL — `solveLookAt` undefined.

- [ ] **Step 4: Implement `solveLookAt` in `Ik.cpp`**

Add before the closing `}  // namespace iron`:

```cpp
Quat solveLookAt(Vec3 bonePos, Vec3 currentForward, Vec3 target,
                 float maxAngleRad) {
    const Vec3 toTarget = target - bonePos;
    if (length(toTarget) < 1e-5f || length(currentForward) < 1e-5f) {
        return Quat::identity();
    }
    const Vec3 desired = normalize(toTarget);
    const Vec3 cur = normalize(currentForward);
    const float d = std::clamp(dot(cur, desired), -1.0f, 1.0f);
    const float angle = std::acos(d);
    if (angle <= maxAngleRad || angle < 1e-5f) {
        return rotationFromTo(cur, desired);
    }
    Vec3 axis = cross(cur, desired);
    if (length(axis) < 1e-5f) return Quat::identity();
    axis = normalize(axis);
    return Quat::fromAxisAngle(axis, maxAngleRad);
}
```

- [ ] **Step 5: Register test**

`tests/CMakeLists.txt` — after `test_two_bone_ik`:
```cmake
iron_add_test(test_look_at test_look_at.cpp)
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build --target test_look_at && ctest --test-dir build -R test_look_at --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/Ik.h engine/asset/Ik.cpp tests/test_look_at.cpp tests/CMakeLists.txt
git commit -m "M51: look-at IK (clamped aim cone)"
```

---

## Task 8: CharacterAnimator cross-fade

Refactor `CharacterAnimator` off the inner `AnimationPlayer` onto its own time + the `Pose` pipeline, and add cross-fade transitions.

**Files:**
- Modify: `engine/asset/CharacterAnimator.h`
- Modify: `engine/asset/CharacterAnimator.cpp`
- Modify: `tests/test_character_animator.cpp` (existing tests must still pass + new fade tests)

- [ ] **Step 1: Replace `CharacterAnimator.h` with the cross-fade-capable version**

```cpp
#pragma once

#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Drives one skinned character with cross-fading state transitions. Owns no
// glTF data (skeleton + clips stay owned by the loader). State names map to
// clips; switchTo(state, fadeTime) blends smoothly. switchTo(state) == hard cut.
class CharacterAnimator {
public:
    void setSkeleton(const Skeleton* skeleton);
    void setClipForState(std::string state, const AnimationClip* clip);

    // Hard cut (fade time 0).
    void switchTo(std::string_view state);
    // Cross-fade to `state` over `fadeTime` seconds. fadeTime <= 0, no prior
    // state, or no skeleton => hard cut. Re-issuing mid-fade re-targets from
    // the current blended pose (no pop).
    void switchTo(std::string_view state, float fadeTime);

    void update(float dt);
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

protected:
    // Sample a state's local pose at `time`. Overridden semantics for blend
    // spaces arrive in a later task; this base looks up single clips.
    void sampleState(std::string_view state, float time, Pose& out) const;
    // Duration used to wrap a state's playback time.
    float stateDuration(std::string_view state) const;
    bool  isKnownState(std::string_view state) const;

    const Skeleton* skeleton_ = nullptr;
    std::unordered_map<std::string, const AnimationClip*> clips_;
    std::string currentState_;
    float       time_ = 0.0f;

    // Cross-fade state.
    bool        fading_ = false;
    float       fadeTime_ = 0.0f;
    float       fadeElapsed_ = 0.0f;
    bool        prevFrozen_ = false;   // true => fade from prevFrozenPose_
    std::string prevState_;            // valid when !prevFrozen_
    float       prevTime_ = 0.0f;
    Pose        prevFrozenPose_;

    mutable Pose lastPose_;            // last evaluated local pose (for retarget)
    bool         warnedUnknownState_ = false;
};

}  // namespace iron
```

- [ ] **Step 2: Replace `CharacterAnimator.cpp` with the cross-fade implementation**

```cpp
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

bool CharacterAnimator::isKnownState(std::string_view state) const {
    return clips_.count(std::string(state)) > 0;
}

float CharacterAnimator::stateDuration(std::string_view state) const {
    auto it = clips_.find(std::string(state));
    if (it == clips_.end() || !it->second) return 0.0f;
    return it->second->duration;
}

void CharacterAnimator::sampleState(std::string_view state, float time,
                                    Pose& out) const {
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
        // Mid-fade: freeze the in-progress blended pose as the new source.
        prevFrozen_ = true;
        prevFrozenPose_ = lastPose_;
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
```

- [ ] **Step 3: Add cross-fade tests to `tests/test_character_animator.cpp`**

Add `#include <algorithm>` and, before `return iron_test_result();`, append:

```cpp
    // Cross-fade with fadeTime 0 behaves as a hard cut (matches Test 3).
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");
        const AnimationClip run  = makeTranslateClip("run");
        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("walk", &walk);
        a.setClipForState("run",  &run);
        a.switchTo("walk");
        a.update(0.5f);
        a.switchTo("run", 0.0f);  // explicit hard cut
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 0.0f);
    }

    // Mid-fade blends between previous and new pose. walk holds x=0 at t=0;
    // run drives x: blend at fade weight 0.5 with run at its own time.
    {
        const Skeleton sk = makeTrivialSkeleton();
        // "stand": bone stays at x=0 (no channel drift at t=0).
        AnimationClip stand;
        stand.name = "stand";
        stand.duration = 1.0f;
        AnimationSampler ss;
        ss.inputs = {0.0f, 1.0f};
        ss.outputs = {0, 0, 0,  0, 0, 0};  // x stays 0
        ss.interpolation = AnimationInterpolation::Linear;
        stand.samplers.push_back(ss);
        AnimationChannel sc;
        sc.targetBone = 0; sc.path = AnimationPath::Translation; sc.samplerIndex = 0;
        stand.channels.push_back(sc);

        const AnimationClip run = makeTranslateClip("run");  // x: 0 -> 10 over 1s

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("stand", &stand);
        a.setClipForState("run", &run);
        a.switchTo("stand");
        a.update(0.1f);             // settle in stand
        a.switchTo("run", 1.0f);    // begin a 1s fade; run time starts at 0
        a.update(0.5f);             // fade weight 0.5; run time 0.5 -> x=5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        // blend(prev stand x=0, cur run x=5, w=0.5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }

    // After the fade completes, only the new state drives the pose.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip stand = makeTranslateClip("a");  // x:0->10 (named "a")
        const AnimationClip run   = makeTranslateClip("b");
        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("stand", &stand);
        a.setClipForState("run", &run);
        a.switchTo("stand");
        a.update(0.2f);
        a.switchTo("run", 0.5f);
        a.update(0.5f);   // fade fully elapses; run time 0.5 -> x=5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 3), 5.0f);  // pure run, no blend
    }
```

- [ ] **Step 4: Build and run (regression + new)**

Run: `cmake --build build --target test_character_animator && ctest --test-dir build -R test_character_animator --output-on-failure`
Expected: PASS — original 5 tests + 3 fade tests.

- [ ] **Step 5: Build the whole engine + all tests (interface-change gate)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — confirms the `AnimationPlayer`-removal from `CharacterAnimator` didn't break any other consumer (net-shooter still links; see Task 11 for the demo wiring).
> Per [[verify-clean-build-before-ci]]: a full build catches stale binaries / interface breaks the incremental target build hides.

- [ ] **Step 6: Commit**

```bash
git add engine/asset/CharacterAnimator.h engine/asset/CharacterAnimator.cpp tests/test_character_animator.cpp
git commit -m "M51: CharacterAnimator cross-fade transitions (own time + Pose pipeline)"
```

---

## Task 9: CharacterAnimator 1D blend space

**Files:**
- Modify: `engine/asset/CharacterAnimator.h`
- Modify: `engine/asset/CharacterAnimator.cpp`
- Modify: `tests/test_character_animator.cpp`

- [ ] **Step 1: Add blend-space API + state to `CharacterAnimator.h`**

Add the include near the top:
```cpp
#include "asset/PoseBlend.h"
```
In the `public:` section after `setClipForState`:
```cpp
    // Register a 1D blend space under a state name (takes precedence over a
    // single clip registered under the same name). Stored by value.
    void setBlendSpaceForState(std::string state, BlendSpace1D space);
    // Drive the active blend space (e.g. movement speed).
    void setBlendParam(float param);
```
In the protected data, after the `clips_` map:
```cpp
    std::unordered_map<std::string, BlendSpace1D> blendSpaces_;
    float blendParam_ = 0.0f;
```

- [ ] **Step 2: Add failing test to `tests/test_character_animator.cpp`**

Before `return iron_test_result();`:
```cpp
    // Blend-space state: param drives the blended output.
    {
        const Skeleton sk = makeTrivialSkeleton();
        const AnimationClip walk = makeTranslateClip("walk");  // x:0->10
        AnimationClip still;
        still.name = "still";
        still.duration = 1.0f;
        AnimationSampler ss;
        ss.inputs = {0.0f, 1.0f};
        ss.outputs = {0, 0, 0, 0, 0, 0};
        ss.interpolation = AnimationInterpolation::Linear;
        still.samplers.push_back(ss);
        AnimationChannel sc;
        sc.targetBone = 0; sc.path = AnimationPath::Translation; sc.samplerIndex = 0;
        still.channels.push_back(sc);

        BlendSpace1D space;
        space.add(0.0f, &still);   // param 0 -> x stays 0
        space.add(1.0f, &walk);    // param 1 -> x up to 10

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setBlendSpaceForState("locomotion", std::move(space));
        a.switchTo("locomotion");
        a.setBlendParam(0.5f);
        a.update(1.0f);            // time wraps to 0 at duration 1 -> sample at end? 
        // update advances time to 1.0, wrapped by duration 1.0 -> 0.0; sample t=0 => x=0.
        // Use a partial step to land mid-clip instead:
        a.setBlendParam(0.5f);
        // Re-evaluate at an explicit mid time by stepping 0.5 from 0.
        // (time_ is 0 after the wrap above; step again.)
        a.update(0.5f);           // time 0.5
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        // blendSpace(param 0.5): blend(still x=0, walk x=5) = 2.5
        CHECK_NEAR(out[0].at(0, 3), 2.5f);
    }
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_character_animator`
Expected: FAIL — `setBlendSpaceForState` / `setBlendParam` undefined.

- [ ] **Step 4: Implement in `CharacterAnimator.cpp`**

Add the two setters (place after `setClipForState`):
```cpp
void CharacterAnimator::setBlendSpaceForState(std::string state,
                                              BlendSpace1D space) {
    blendSpaces_[std::move(state)] = std::move(space);
}

void CharacterAnimator::setBlendParam(float param) {
    blendParam_ = param;
}
```
Extend `isKnownState` to recognize blend spaces:
```cpp
bool CharacterAnimator::isKnownState(std::string_view state) const {
    return clips_.count(std::string(state)) > 0 ||
           blendSpaces_.count(std::string(state)) > 0;
}
```
Extend `stateDuration` — blend space uses its first sample's clip duration:
```cpp
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
```
Extend `sampleState` to prefer a blend space:
```cpp
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
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build --target test_character_animator && ctest --test-dir build -R test_character_animator --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/asset/CharacterAnimator.h engine/asset/CharacterAnimator.cpp tests/test_character_animator.cpp
git commit -m "M51: CharacterAnimator 1D blend space (speed-driven gait)"
```

---

## Task 10: CharacterAnimator IK config + application

Add two-bone + look-at solver configuration and apply them in `evaluate` on the global transforms (between compose and palette).

**Files:**
- Modify: `engine/asset/CharacterAnimator.h`
- Modify: `engine/asset/CharacterAnimator.cpp`
- Modify: `tests/test_character_animator.cpp`

- [ ] **Step 1: Add IK API + state to `CharacterAnimator.h`**

Add include:
```cpp
#include "asset/Ik.h"
```
Add `#include <vector>` if not already present (it is via `<unordered_map>` transitively, but add explicitly).
In `public:` after `setBlendParam`:
```cpp
    // Configure a two-bone IK chain (returns a handle). Target is supplied per
    // frame via setIKTarget. weight in [0,1] blends the solve in/out.
    int  addTwoBoneIK(int rootBone, int midBone, int endBone, Vec3 pole,
                      float weight);
    void setIKTarget(int handle, Vec3 worldTarget);
    void setIKWeight(int handle, float weight);

    // Configure a look-at on a single bone. forwardAxis is the bone's local
    // forward (in bone space); maxAngle clamps the turn. Returns a handle.
    int  addLookAt(int bone, Vec3 forwardAxis, float maxAngle, float weight);
    void setLookAtTarget(int handle, Vec3 worldTarget);
    void setLookAtWeight(int handle, float weight);
```
In protected data after `blendParam_`:
```cpp
    struct TwoBoneIK {
        int  root, mid, end;
        Vec3 pole;
        Vec3 target{0, 0, 0};
        float weight = 1.0f;
        bool hasTarget = false;
    };
    struct LookAt {
        int  bone;
        Vec3 forwardAxis;
        float maxAngle;
        Vec3 target{0, 0, 0};
        float weight = 1.0f;
        bool hasTarget = false;
    };
    std::vector<TwoBoneIK> twoBoneIK_;
    std::vector<LookAt>    lookAts_;
```
Add a protected helper declaration:
```cpp
    // Apply all configured IK solvers to global bone transforms in place.
    void applyIK(std::span<Mat4> globals) const;
```

- [ ] **Step 2: Add failing IK tests to `tests/test_character_animator.cpp`**

Add `#include "asset/Ik.h"` and before `return iron_test_result();`:
```cpp
    // Look-at rotates the configured bone toward the target. Two-bone chain:
    // root(0) at origin, child(1) head offset +X. With forwardAxis +X and a
    // target at +Z, the head bone's global orientation should turn toward +Z.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        Bone head;
        head.parentIndex = 0;
        head.localBindTransform = composeTRS(Vec3{0, 1, 0}, Quat::identity(), Vec3{1, 1, 1});
        head.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);
        sk.bones.push_back(head);

        AnimationClip idle;  // no channels -> bind pose
        idle.name = "idle";
        idle.duration = 1.0f;

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("idle", &idle);
        a.switchTo("idle");

        const int h = a.addLookAt(1, Vec3{1, 0, 0}, 3.14159f, 1.0f);
        a.setLookAtTarget(h, Vec3{1 + 1, 0, 5});  // off to +Z from the head at (0,1,0)... see note
        a.update(0.016f);

        std::array<Mat4, 2> palette{};
        std::array<Mat4, 2> noik{};
        // Baseline without target: same call but weight 0.
        a.setLookAtWeight(h, 0.0f);
        a.evaluate(noik);
        a.setLookAtWeight(h, 1.0f);
        a.evaluate(palette);
        // With look-at active, the head palette differs from the no-IK palette.
        bool differs = false;
        for (int i = 0; i < 16; ++i)
            if (std::fabs(palette[1].m[i] - noik[1].m[i]) > 1e-3f) differs = true;
        CHECK(differs);
    }

    // IK weight 0 == passthrough (palette equals the no-IK palette).
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        AnimationClip idle;
        idle.name = "idle";
        idle.duration = 1.0f;

        CharacterAnimator a;
        a.setSkeleton(&sk);
        a.setClipForState("idle", &idle);
        a.switchTo("idle");
        const int h = a.addLookAt(0, Vec3{1, 0, 0}, 3.14159f, 0.0f);
        a.setLookAtTarget(h, Vec3{0, 0, 5});
        std::array<Mat4, 1> out{};
        a.evaluate(out);
        CHECK_NEAR(out[0].at(0, 0), 1.0f);  // unchanged identity
        CHECK_NEAR(out[0].at(1, 1), 1.0f);
        CHECK_NEAR(out[0].at(2, 2), 1.0f);
    }
```
Add `#include "asset/Pose.h"` to the test for `composeTRS` (already pulled in transitively via CharacterAnimator.h, but make it explicit).

> Note for the implementer: the look-at "differs" assertion only needs the head palette to change when the solver is active; exact orientation is validated in `test_look_at`. Adjust the target vector if needed so the head actually turns (any target not along the bone's current forward works).

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target test_character_animator`
Expected: FAIL — IK methods undefined.

- [ ] **Step 4: Implement IK config + application in `CharacterAnimator.cpp`**

Add includes:
```cpp
#include "math/Vec.h"
```
Add the config methods (after `setBlendParam`):
```cpp
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
```
Add the `applyIK` helper. It rotates a bone (and its descendants) about a world pivot by a weighted delta:
```cpp
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

void CharacterAnimator::applyIK(std::span<Mat4> globals) const {
    if (!skeleton_) return;
    const int n = static_cast<int>(std::min(globals.size(),
                                            skeleton_->bones.size()));

    // returns true if `bone` is `ancestor` or a descendant of it.
    auto inSubtree = [&](int bone, int ancestor) {
        for (int b = bone; b >= 0; b = skeleton_->bones[b].parentIndex) {
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
```
Add `#include <algorithm>` (already present from Task 8). Now route `evaluate` through `composeGlobals` → `applyIK` → `globalsToPalette` (replace the `posePalette` call):
```cpp
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

    const std::size_t n = std::min(out.size(), skeleton_->bones.size());
    std::vector<Mat4> globals(n);
    composeGlobals(*skeleton_, pose, globals);
    applyIK(globals);
    globalsToPalette(*skeleton_, globals, out);
}
```
Add `#include <vector>` to the `.cpp`.

- [ ] **Step 5: Build and run**

Run: `cmake --build build --target test_character_animator && ctest --test-dir build -R test_character_animator --output-on-failure`
Expected: PASS — all prior tests + look-at-differs + weight-0-passthrough.

- [ ] **Step 6: Full build + full test sweep**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/CharacterAnimator.h engine/asset/CharacterAnimator.cpp tests/test_character_animator.cpp
git commit -m "M51: CharacterAnimator IK (two-bone + look-at applied on globals)"
```

---

## Task 11: Net-shooter demo — cross-fades, speed blend space, head look-at

Wire the new features into the fox rendering. The demo has no unit test; it is validated by the visual gate.

**Files:**
- Modify: `games/07-net-shooter/main.cpp` (the `submitPlayerFox` lambda + animator setup, around lines 2083–2115)

- [ ] **Step 1: Replace the per-fox setup with blend-space + look-at config**

In the `if (inserted)` block that initializes a new animator (currently `setClipForState("idle"/"walk"/"run")`), replace with a locomotion blend space plus a head look-at. First locate the fox head/neck bone index once (after the model loads), near where `foxModel` is available:

```cpp
// One-time: find the fox's head/neck bone for look-at (by name; falls back
// to -1 -> look-at simply isn't added).
int foxHeadBone = -1;
{
    const auto& bones = foxModel->skinnedMesh->skeleton.bones;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        // Fox.glb names the head bone "b_Head_05" (neck is "b_Neck_04");
        // match case-insensitively on "head", else "neck".
        std::string n = bones[i].name;
        for (auto& c : n) c = static_cast<char>(std::tolower(c));
        if (n.find("head") != std::string::npos) { foxHeadBone = static_cast<int>(i); break; }
        if (n.find("neck") != std::string::npos) foxHeadBone = static_cast<int>(i);
    }
}
```
> The implementer should confirm the actual bone name by dumping `skeleton.bones[i].name` once (a temporary `Log::info`) and pick the head/neck bone; the substring match above is the default.

Then in `submitPlayerFox`'s `if (inserted)` block:
```cpp
if (inserted) {
    auto& anim = animIt->second;
    anim.setSkeleton(&foxModel->skinnedMesh->skeleton);

    // 1D locomotion blend space driven by horizontal speed.
    iron::BlendSpace1D loco;
    if (foxIdleClip) loco.add(0.0f, foxIdleClip);   // standing
    if (foxWalkClip) loco.add(2.5f, foxWalkClip);   // walk around mid-speed
    if (foxRunClip)  loco.add(6.0f, foxRunClip);    // run at sprint speed
    anim.setBlendSpaceForState("locomotion", std::move(loco));

    // Keep the old discrete states too (idle in air, etc.).
    anim.setClipForState("idle", foxIdleClip);

    // Head look-at toward the aim direction. Fox forward (post +180 yaw) is
    // -Z in model space; the local forward axis for the head bone is +Z here
    // because the model is rotated. Use +Z and a 70-degree clamp.
    if (foxHeadBone >= 0) {
        const int h = anim.addLookAt(foxHeadBone, iron::Vec3{0, 0, 1},
                                     1.22f /* ~70 deg */, 0.7f);
        animLookAtHandles[pid] = h;
    }
}
```
Add a companion map near `playerAnimators`:
```cpp
std::unordered_map<std::uint32_t, int> animLookAtHandles;
```

- [ ] **Step 2: Replace discrete state selection with blend-space + cross-fade + look-at target**

Replace the `const char* state = ...; switchTo(state); update(...)` block with:
```cpp
const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);

// In the air -> idle (no jump anim); grounded -> blended locomotion.
auto& anim = animIt->second;
if (!grounded) {
    anim.switchTo("idle", 0.15f);
} else {
    anim.switchTo("locomotion", 0.15f);
    anim.setBlendParam(speed);
}

// Aim the head along the look yaw, a few units ahead at head height.
auto lh = animLookAtHandles.find(pid);
if (lh != animLookAtHandles.end()) {
    const float aimYaw = yawRadians;  // networked aim
    const iron::Vec3 ahead{
        pos.x - std::sin(aimYaw) * 4.0f,
        pos.y + 1.2f,
        pos.z - std::cos(aimYaw) * 4.0f};
    anim.setLookAtTarget(lh->second, ahead);
}

anim.update(frameDt);
```
> The `ahead` vector points along the player's aim yaw; tune the `-sin/-cos` signs and the `+180` interplay so the head turns the correct way (the fox model is yaw+180). The implementer should eyeball this during the visual gate and flip a sign if the head looks backward.

- [ ] **Step 3: Build the game**

Run: `cmake --build build --target 07-net-shooter` (use the actual target name; confirm via `cmake --build build --target help | grep -i net-shooter` if unsure)
Expected: links cleanly.

- [ ] **Step 4: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M51: net-shooter demo — speed blend space, cross-fades, head look-at"
```

---

## Task 12: Update progress memory

**Files:**
- Modify: `C:\Users\elias\.claude\projects\C--Users-elias-Documents--dev-iron-core-engine\memory\iron-core-engine-progress.md`
- Modify: `C:\Users\elias\.claude\projects\C--Users-elias-Documents--dev-iron-core-engine\memory\MEMORY.md`

- [ ] **Step 1: Record M51 in the progress memory and index**

Append an M51 entry to the progress memory (Pose/blend/IK summary, files, PR number once known) and add a one-line pointer in `MEMORY.md` if a dedicated animation memory is warranted. Do this AFTER the visual gate passes and the PR is opened (so the PR number is real). No code; documentation only.

- [ ] **Step 2: Commit (memory lives outside the repo; no git commit needed — these are tool-written files)**

Skip git for memory files. This step is a reminder to update memory at milestone close.

---

## Visual Gate (after Task 11, before final review)

Run the net-shooter sandbox and confirm with the user:
- Idle ↔ walk ↔ run **cross-fade smoothly** (no snapping at state changes).
- Gait **blends with speed** (slow → walk, fast → run, in-between blends).
- The fox **head tracks** the aim direction (turns toward where the player looks, clamped ~70°, snaps back within range).
- No popping/jitter; nothing NaNs; foxes that stand, walk, run, and jump all look correct.

If anything reads wrong (head turns the wrong way, blend thresholds off), iterate on the demo tuning in Task 11 (sign flips, blend-space param values) — these are demo-side, not solver bugs (solvers are unit-locked).

---

## Self-Review (completed by plan author)

**1. Spec coverage:**
- Pose foundation (BoneLocal/Pose, samplePose, posePalette, Quat reuse) → Tasks 1–3. ✓
- Blending: blendPose + cross-fade + 1D blend space → Tasks 4, 5, 8, 9. ✓
- IK: two-bone + look-at on globals, configured on CharacterAnimator → Tasks 6, 7, 10. ✓
- Demo (net-shooter cross-fade + speed blend + look-at) → Task 11. ✓
- Tests: test_pose(+blend), test_pose_blend, test_two_bone_ik, test_look_at, test_character_animator extensions → Tasks 1–10. (Spec listed `test_quat`; `Quat`/`slerp` is pre-existing and already covered by `test_quaternion` — no new quat test needed; noted in the plan.) ✓
- Out-of-scope items (additive/FABRIK/2D blend/root motion) are not implemented. ✓

**2. Placeholder scan:** No TBD/TODO. Two demo-tuning notes (head bone name, look-at sign) are explicit "confirm at build time" items with concrete defaults, matching the spec's "two-bone IK firmed at build time" intent — not blockers. The two-bone IK chain on the fox is left to the visual-gate phase (look-at is the primary IK demo); the solver itself is fully implemented + unit-tested.

**3. Type consistency:** `Pose`/`BoneLocal`, `composeTRS`/`decomposeTRS`/`composeGlobals`/`globalsToPalette`/`posePalette`, `blendPose`/`BlendSpace1D::add`/`sampleBlendSpace`, `TwoBoneIKResult`/`solveTwoBoneIK`/`rotationFromTo`/`solveLookAt`, and `CharacterAnimator::{switchTo(2-arg),setBlendSpaceForState,setBlendParam,addTwoBoneIK,setIKTarget,setIKWeight,addLookAt,setLookAtTarget,setLookAtWeight,applyIK}` are used consistently across tasks. Existing `Quat::slerp`/`fromAxisAngle`/`toMat4`/`rotate`, `Vec3` `interpolate`/`normalize`/`cross`/`dot`/`length`, and `Mat4::at` match the real headers read during planning. ✓
