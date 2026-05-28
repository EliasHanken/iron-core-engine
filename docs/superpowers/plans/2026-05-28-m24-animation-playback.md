# M24: glTF Animation Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load glTF animation channels into engine data structures, sample them at runtime into a bone-matrix palette, and replace `10-gltf-viewer`'s static bind-pose with a looping animation.

**Architecture:**
- `Animation.h` defines pure data: `AnimationSampler` (keyframe times + values + interp mode), `AnimationChannel` (target bone + path + sampler index), `AnimationClip` (name + duration + samplers + channels).
- `GltfLoader` parses `model.animations[]`, maps target node IDs → bone indices using the existing node→bone map built during M23 skeleton load. Unsupported features (CubicSpline interp, morph-target "weights" path) are dropped with a warn-once log.
- `AnimationPlayer` owns playback state: a borrowed `Skeleton*`, a borrowed `AnimationClip*`, current time. `update(dt)` advances + loops; `evaluate(std::span<Mat4>)` samples each channel into per-bone TRS overrides, composes local matrices, walks the hierarchy in parent-first order, then multiplies by `inverseBindMatrix` to write the final palette. Per-bone defaults come from `Bone::localBindTransform` (used directly when a bone has no channel).
- `Quat::slerp` lands in `Quaternion.h` (with the standard "dot-flip + lerp-fallback for tiny angle" pattern).
- `10-gltf-viewer` constructs an `AnimationPlayer`, calls `update(dt)` per frame, replaces the identity-fill with `player.evaluate(bonesPose)`.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), Vulkan 1.3, tinygltf (already a dep), CMake, CTest. Builds on M23 (PR #42 — `Skeleton`, `SkinnedMeshData`, `SkinnedDrawCall`, `kMaxBonesPerSkinnedMesh`, the existing parent→child node walk in GltfLoader).

**Prerequisite:** PR #42 (M23) must be merged to `main`. Branch this work from `main` after that merge.

---

## File Structure

**Create:**
- `engine/asset/Animation.h` — POD types: `AnimationInterpolation`, `AnimationPath`, `AnimationSampler`, `AnimationChannel`, `AnimationClip`.
- `engine/asset/AnimationPlayer.h` — Playback class declaration.
- `engine/asset/AnimationPlayer.cpp` — Sampling + bone-matrix composition.
- `tests/test_animation_player.cpp` — Unit tests for sampler math, slerp, composition, loop wraparound.

**Modify:**
- `engine/math/Quaternion.h` — Add `Quat::slerp(const Quat& a, const Quat& b, float t)` and a `dot(Quat, Quat)` free function.
- `engine/asset/GltfLoader.h` — Add `#include "asset/Animation.h"`; extend `GltfModel` with `std::vector<AnimationClip> animations`.
- `engine/asset/GltfLoader.cpp` — Parse `model.animations[]` after skin/skeleton parsing. Reuse the existing `nodeIndexToBoneIndex` map from M23. Warn-once on CubicSpline, warn-once on `weights` channels.
- `engine/CMakeLists.txt` — Add `engine/asset/AnimationPlayer.cpp` to the engine target sources.
- `tests/CMakeLists.txt` — Add `test_animation_player.cpp`.
- `tests/test_quaternion.cpp` — Add slerp tests (interpolation endpoints, halfway, antipodal-shortest-path).
- `tests/test_gltf_loader.cpp` — Assert RiggedSimple now loads exactly 1 animation with the expected channel count.
- `games/10-gltf-viewer/main.cpp` — Construct `AnimationPlayer`, drive `bonesPose` per frame.
- `docs/engine/asset-pipeline.md` — Append M24 section.

**Out of scope (explicit non-goals):** CubicSpline interpolation, morph-target weights, animation blending, animation events, retargeting, separate state machine. These are M26+ topics; v1 is "one clip, one player, looped."

---

## Task 1: Quaternion slerp

**Files:**
- Modify: `engine/math/Quaternion.h`
- Modify: `tests/test_quaternion.cpp`

- [ ] **Step 1: Write failing tests in `tests/test_quaternion.cpp`**

Append (in the same TEST-macro style already in the file — check the existing top of the file for the harness):

```cpp
TEST(QuatSlerp_Endpoints) {
    const Quat a = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.0f);
    const Quat b = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5f);
    const Quat r0 = Quat::slerp(a, b, 0.0f);
    const Quat r1 = Quat::slerp(a, b, 1.0f);
    EXPECT_NEAR(r0.x, a.x, 1e-5f); EXPECT_NEAR(r0.y, a.y, 1e-5f);
    EXPECT_NEAR(r0.z, a.z, 1e-5f); EXPECT_NEAR(r0.w, a.w, 1e-5f);
    EXPECT_NEAR(r1.x, b.x, 1e-5f); EXPECT_NEAR(r1.y, b.y, 1e-5f);
    EXPECT_NEAR(r1.z, b.z, 1e-5f); EXPECT_NEAR(r1.w, b.w, 1e-5f);
}

TEST(QuatSlerp_Halfway) {
    const Quat a = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.0f);
    const Quat b = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.0f);
    const Quat mid = Quat::slerp(a, b, 0.5f);
    const Quat expected = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.5f);
    EXPECT_NEAR(mid.x, expected.x, 1e-5f); EXPECT_NEAR(mid.y, expected.y, 1e-5f);
    EXPECT_NEAR(mid.z, expected.z, 1e-5f); EXPECT_NEAR(mid.w, expected.w, 1e-5f);
}

TEST(QuatSlerp_TakesShortPath) {
    // a and -a represent the same rotation; slerp must pick the short arc.
    const Quat a{0.0f, 0.7071f, 0.0f, 0.7071f};
    const Quat negA{-a.x, -a.y, -a.z, -a.w};
    const Quat mid = Quat::slerp(a, negA, 0.5f);
    // Halfway between a and -a along the short arc is a itself (zero rotation diff).
    EXPECT_NEAR(std::abs(mid.x), std::abs(a.x), 1e-4f);
    EXPECT_NEAR(std::abs(mid.w), std::abs(a.w), 1e-4f);
}
```

(The exact `TEST(...)` / `EXPECT_NEAR` macros come from `tests/test_framework.h` — verify the existing tests in `test_quaternion.cpp` use the same names before pasting.)

- [ ] **Step 2: Run tests to verify they fail**

```powershell
cmake --build build-vk --target test_quaternion --config Debug
ctest --test-dir build-vk -R quaternion --output-on-failure
```

Expected: compile error referencing `Quat::slerp` (function not defined).

- [ ] **Step 3: Implement slerp in `engine/math/Quaternion.h`**

Add inside the `iron` namespace, after the existing `operator*`:

```cpp
constexpr float dot(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

inline Quat Quat::slerp(const Quat& a, const Quat& b, float t) {
    // Pick the shorter arc by flipping `b` if the dot product is negative.
    float d = dot(a, b);
    Quat b2 = b;
    if (d < 0.0f) {
        b2 = Quat{-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    // For very small angles, fall back to nlerp to avoid div-by-zero.
    if (d > 0.9995f) {
        Quat r{
            a.x + t * (b2.x - a.x),
            a.y + t * (b2.y - a.y),
            a.z + t * (b2.z - a.z),
            a.w + t * (b2.w - a.w),
        };
        return r.normalized();
    }
    const float theta0 = std::acos(d);
    const float theta = theta0 * t;
    const float sinTheta = std::sin(theta);
    const float sinTheta0 = std::sin(theta0);
    const float s0 = std::cos(theta) - d * sinTheta / sinTheta0;
    const float s1 = sinTheta / sinTheta0;
    return Quat{
        s0 * a.x + s1 * b2.x,
        s0 * a.y + s1 * b2.y,
        s0 * a.z + s1 * b2.z,
        s0 * a.w + s1 * b2.w,
    };
}
```

And add the declaration inside `struct Quat` (after `rotate`):

```cpp
static Quat slerp(const Quat& a, const Quat& b, float t);
```

- [ ] **Step 4: Run tests to verify they pass**

```powershell
cmake --build build-vk --target test_quaternion --config Debug
ctest --test-dir build-vk -R quaternion --output-on-failure
```

Expected: all `QuatSlerp_*` tests pass, no existing tests break.

- [ ] **Step 5: Commit**

```bash
git add engine/math/Quaternion.h tests/test_quaternion.cpp
git commit -m "M24 Task 1: add Quat::slerp with shortest-arc handling"
```

---

## Task 2: Animation data types

**Files:**
- Create: `engine/asset/Animation.h`

- [ ] **Step 1: Create `engine/asset/Animation.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// glTF animation sampler interpolation modes.
//   Linear  — vec/quat interpolated (slerp for rotations).
//   Step    — pick previous keyframe's value.
//   CubicSpline — Hermite cubic. Not implemented in M24; loader warns and
//                  downgrades the sampler to Linear at load time.
enum class AnimationInterpolation : std::uint8_t {
    Linear = 0,
    Step,
    CubicSpline,
};

// What a channel drives on its target bone.
//   Translation/Scale — output is 3 floats per keyframe (Vec3).
//   Rotation          — output is 4 floats per keyframe (Quat xyzw).
//   Weights (morph targets) — not supported in M24; loader skips these
//                              channels and warns once.
enum class AnimationPath : std::uint8_t {
    Translation = 0,
    Rotation,
    Scale,
};

// Keyframe data for one transform component on one bone.
//   inputs  — strictly increasing timestamps in seconds.
//   outputs — packed values: 3 floats per keyframe for T/S, 4 for R.
//   The loader normalizes outputs to start at t=0 (does NOT subtract the
//   first keyframe time — glTF spec allows non-zero start; sampling code
//   handles that by clamping to inputs.front() before inputs.front()).
struct AnimationSampler {
    std::vector<float>     inputs;
    std::vector<float>     outputs;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

// A channel binds a sampler's output stream to one (bone, path) pair.
// targetBone == -1 means "channel was dropped during load" (target node
// has no corresponding bone, or path was unsupported). Player code skips
// channels with targetBone < 0.
struct AnimationChannel {
    int           targetBone   = -1;
    AnimationPath path         = AnimationPath::Translation;
    int           samplerIndex = -1;
};

// One named animation clip. `duration` is max(sampler.inputs.back()) across
// every sampler, computed by the loader. Playback wraps at this duration.
struct AnimationClip {
    std::string                   name;
    float                         duration = 0.0f;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

}  // namespace iron
```

- [ ] **Step 2: Verify it compiles**

```powershell
cmake --build build-vk --target iron_engine --config Debug
```

Expected: clean build (this file is header-only and not yet included anywhere; it will be picked up in Task 3).

- [ ] **Step 3: Commit**

```bash
git add engine/asset/Animation.h
git commit -m "M24 Task 2: add AnimationClip/Sampler/Channel POD types"
```

---

## Task 3: AnimationPlayer (TDD core)

**Files:**
- Create: `engine/asset/AnimationPlayer.h`
- Create: `engine/asset/AnimationPlayer.cpp`
- Create: `tests/test_animation_player.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Declare `AnimationPlayer` in `engine/asset/AnimationPlayer.h`**

```cpp
#pragma once

#include "asset/Animation.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>

namespace iron {

// Drives one AnimationClip over a Skeleton. Player does NOT own its
// inputs — both pointers are non-owning. Pass nullptrs to disable.
class AnimationPlayer {
public:
    // Bind the skeleton and the clip to play. Either may be nullptr.
    // Resets time to zero.
    void setSkeleton(const Skeleton* skeleton);
    void setClip(const AnimationClip* clip);

    // Advance playback time by dt seconds, wrapping at clip duration.
    // No-op if either skeleton or clip is null, or clip duration is zero.
    void update(float dt);

    // Write the final bone-matrix palette into `out` in skeleton order.
    // Bones beyond out.size() are dropped. Unwritten slots (out.size() >
    // skeleton bone count) are left untouched — caller must initialize.
    //
    // If no skeleton is set, this is a no-op. If a skeleton is set but no
    // clip, writes the bind-pose palette (matrix per bone =
    // globalBindTransform * inverseBindMatrix).
    void evaluate(std::span<Mat4> out) const;

    float time() const { return time_; }
    void  setTime(float t) { time_ = t; }

private:
    const Skeleton*      skeleton_ = nullptr;
    const AnimationClip* clip_     = nullptr;
    float                time_     = 0.0f;
};

}  // namespace iron
```

- [ ] **Step 2: Write failing tests in `tests/test_animation_player.cpp`**

```cpp
#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <array>
#include <cmath>

using namespace iron;

namespace {

// Single-bone skeleton at origin, identity bind transform, identity inverseBind.
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

// One-bone clip: rotate bone 0 from identity to 90deg about Y over 1.0s.
AnimationClip makeRotateClip() {
    AnimationClip clip;
    clip.name = "spin";
    clip.duration = 1.0f;

    AnimationSampler samp;
    samp.inputs  = {0.0f, 1.0f};
    const Quat q0 = Quat::identity();
    const Quat q1 = Quat::fromAxisAngle(Vec3{0, 1, 0}, 3.14159265f * 0.5f);
    samp.outputs = {q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w};
    samp.interpolation = AnimationInterpolation::Linear;
    clip.samplers.push_back(samp);

    AnimationChannel ch;
    ch.targetBone   = 0;
    ch.path         = AnimationPath::Rotation;
    ch.samplerIndex = 0;
    clip.channels.push_back(ch);

    return clip;
}

}  // namespace

TEST(AnimationPlayer_NoSkeleton_NoOp) {
    AnimationPlayer p;
    std::array<Mat4, 4> out{};
    for (auto& m : out) m = Mat4::identity();
    p.evaluate(out);
    EXPECT_NEAR(out[0].at(0, 0), 1.0f, 1e-6f);
}

TEST(AnimationPlayer_BindPoseWhenNoClip) {
    const Skeleton sk = makeTrivialSkeleton();
    AnimationPlayer p;
    p.setSkeleton(&sk);
    std::array<Mat4, 1> out{};
    p.evaluate(out);
    // Trivial skeleton: identity * identity = identity.
    EXPECT_NEAR(out[0].at(0, 0), 1.0f, 1e-5f);
    EXPECT_NEAR(out[0].at(1, 1), 1.0f, 1e-5f);
    EXPECT_NEAR(out[0].at(2, 2), 1.0f, 1e-5f);
}

TEST(AnimationPlayer_ClipAtTimeZero_BindPose) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip clip = makeRotateClip();
    AnimationPlayer p;
    p.setSkeleton(&sk);
    p.setClip(&clip);
    p.setTime(0.0f);
    std::array<Mat4, 1> out{};
    p.evaluate(out);
    // Identity rotation at t=0.
    EXPECT_NEAR(out[0].at(0, 0), 1.0f, 1e-5f);
    EXPECT_NEAR(out[0].at(2, 2), 1.0f, 1e-5f);
}

TEST(AnimationPlayer_ClipAtEnd_FullRotation) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip clip = makeRotateClip();
    AnimationPlayer p;
    p.setSkeleton(&sk);
    p.setClip(&clip);
    p.setTime(1.0f);
    std::array<Mat4, 1> out{};
    p.evaluate(out);
    // 90deg about Y: m00 = cos(90) = 0, m02 = sin(90) = 1.
    EXPECT_NEAR(out[0].at(0, 0), 0.0f, 1e-4f);
    EXPECT_NEAR(out[0].at(0, 2), 1.0f, 1e-4f);
}

TEST(AnimationPlayer_UpdateLoops) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip clip = makeRotateClip();
    AnimationPlayer p;
    p.setSkeleton(&sk);
    p.setClip(&clip);
    p.setTime(0.0f);
    p.update(2.5f);  // Should wrap to 0.5s.
    EXPECT_NEAR(p.time(), 0.5f, 1e-5f);
}

TEST(AnimationPlayer_StepInterp_HoldsPrevKeyframe) {
    const Skeleton sk = makeTrivialSkeleton();
    AnimationClip clip;
    clip.name = "step";
    clip.duration = 2.0f;
    AnimationSampler samp;
    samp.inputs = {0.0f, 1.0f, 2.0f};
    // Translation along X: 0, 5, 10.
    samp.outputs = {0,0,0,  5,0,0,  10,0,0};
    samp.interpolation = AnimationInterpolation::Step;
    clip.samplers.push_back(samp);
    AnimationChannel ch{0, AnimationPath::Translation, 0};
    clip.channels.push_back(ch);

    AnimationPlayer p;
    p.setSkeleton(&sk);
    p.setClip(&clip);
    p.setTime(0.5f);  // Between kf0 and kf1 → Step holds kf0 (x = 0).
    std::array<Mat4, 1> out{};
    p.evaluate(out);
    EXPECT_NEAR(out[0].at(0, 3), 0.0f, 1e-5f);

    p.setTime(1.99f);  // Just before kf2 → holds kf1 (x = 5).
    p.evaluate(out);
    EXPECT_NEAR(out[0].at(0, 3), 5.0f, 1e-5f);
}
```

- [ ] **Step 3: Add the new files to CMake**

In `engine/CMakeLists.txt`, find the source list that includes `engine/asset/GltfLoader.cpp` and add `engine/asset/AnimationPlayer.cpp` next to it. (If sources are listed alphabetically, put it before GltfLoader; otherwise match the surrounding style.)

In `tests/CMakeLists.txt`, find where `test_gltf_loader.cpp` is registered (likely an `add_executable` per-test or an `iron_add_test` macro call) and add `test_animation_player.cpp` the same way. Look at the existing line for `test_quaternion.cpp` for the exact incantation.

- [ ] **Step 4: Run tests to verify they fail**

```powershell
cmake --build build-vk --target test_animation_player --config Debug
ctest --test-dir build-vk -R animation_player --output-on-failure
```

Expected: link error (AnimationPlayer methods unresolved) OR all 6 tests fail with stub implementation.

- [ ] **Step 5: Implement `engine/asset/AnimationPlayer.cpp`**

```cpp
#include "asset/AnimationPlayer.h"

#include "math/Quaternion.h"
#include "math/Vec.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace iron {

namespace {

// Composes a column-major TRS matrix: M = T * R * S.
Mat4 composeTRS(Vec3 t, Quat r, Vec3 s) {
    Mat4 m = r.toMat4();
    // Scale columns 0..2 by s.
    m.at(0, 0) *= s.x; m.at(1, 0) *= s.x; m.at(2, 0) *= s.x;
    m.at(0, 1) *= s.y; m.at(1, 1) *= s.y; m.at(2, 1) *= s.y;
    m.at(0, 2) *= s.z; m.at(1, 2) *= s.z; m.at(2, 2) *= s.z;
    // Translation in column 3.
    m.at(0, 3) = t.x; m.at(1, 3) = t.y; m.at(2, 3) = t.z;
    m.at(3, 3) = 1.0f;
    return m;
}

// Decompose a column-major Mat4 (assumed TRS, no shear) into T/R/S.
// Rotation extraction uses the standard matrix-to-quaternion trace method.
// Scale recovery assumes positive uniform-or-not scale (no reflections).
void decomposeTRS(const Mat4& m, Vec3& t, Quat& r, Vec3& s) {
    t = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    const Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = Vec3{length(c0), length(c1), length(c2)};
    // Build a pure rotation matrix and convert to quaternion.
    Mat4 rot = Mat4::identity();
    if (s.x > 1e-8f) { rot.at(0,0) = c0.x / s.x; rot.at(1,0) = c0.y / s.x; rot.at(2,0) = c0.z / s.x; }
    if (s.y > 1e-8f) { rot.at(0,1) = c1.x / s.y; rot.at(1,1) = c1.y / s.y; rot.at(2,1) = c1.z / s.y; }
    if (s.z > 1e-8f) { rot.at(0,2) = c2.x / s.z; rot.at(1,2) = c2.y / s.z; rot.at(2,2) = c2.z / s.z; }
    const float trace = rot.at(0,0) + rot.at(1,1) + rot.at(2,2);
    if (trace > 0.0f) {
        const float sq = std::sqrt(trace + 1.0f) * 2.0f;
        r.w = 0.25f * sq;
        r.x = (rot.at(2,1) - rot.at(1,2)) / sq;
        r.y = (rot.at(0,2) - rot.at(2,0)) / sq;
        r.z = (rot.at(1,0) - rot.at(0,1)) / sq;
    } else if (rot.at(0,0) > rot.at(1,1) && rot.at(0,0) > rot.at(2,2)) {
        const float sq = std::sqrt(1.0f + rot.at(0,0) - rot.at(1,1) - rot.at(2,2)) * 2.0f;
        r.w = (rot.at(2,1) - rot.at(1,2)) / sq;
        r.x = 0.25f * sq;
        r.y = (rot.at(0,1) + rot.at(1,0)) / sq;
        r.z = (rot.at(0,2) + rot.at(2,0)) / sq;
    } else if (rot.at(1,1) > rot.at(2,2)) {
        const float sq = std::sqrt(1.0f + rot.at(1,1) - rot.at(0,0) - rot.at(2,2)) * 2.0f;
        r.w = (rot.at(0,2) - rot.at(2,0)) / sq;
        r.x = (rot.at(0,1) + rot.at(1,0)) / sq;
        r.y = 0.25f * sq;
        r.z = (rot.at(1,2) + rot.at(2,1)) / sq;
    } else {
        const float sq = std::sqrt(1.0f + rot.at(2,2) - rot.at(0,0) - rot.at(1,1)) * 2.0f;
        r.w = (rot.at(1,0) - rot.at(0,1)) / sq;
        r.x = (rot.at(0,2) + rot.at(2,0)) / sq;
        r.y = (rot.at(1,2) + rot.at(2,1)) / sq;
        r.z = 0.25f * sq;
    }
    r = r.normalized();
}

// Find keyframe index `i` such that inputs[i] <= t < inputs[i+1].
// Returns std::size_t(-1) if t < inputs.front() (caller clamps).
// Returns inputs.size()-1 if t >= inputs.back() (caller clamps).
std::size_t findKeyframe(const std::vector<float>& inputs, float t) {
    if (inputs.empty()) return static_cast<std::size_t>(-1);
    if (t <= inputs.front()) return 0;
    if (t >= inputs.back()) return inputs.size() - 1;
    // Binary search (inputs strictly increasing per glTF spec).
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
    const Vec3 v1{s.outputs[(i + 1) * 3 + 0], s.outputs[(i + 1) * 3 + 1], s.outputs[(i + 1) * 3 + 2]};
    const float t0 = s.inputs[i];
    const float t1 = s.inputs[i + 1];
    const float a = (t - t0) / (t1 - t0);
    return Vec3{v0.x + (v1.x - v0.x) * a,
                v0.y + (v1.y - v0.y) * a,
                v0.z + (v1.z - v0.z) * a};
}

Quat sampleQuat(const AnimationSampler& s, float t) {
    const std::size_t i = findKeyframe(s.inputs, t);
    if (i == static_cast<std::size_t>(-1)) return Quat::identity();
    const Quat q0{s.outputs[i * 4 + 0], s.outputs[i * 4 + 1], s.outputs[i * 4 + 2], s.outputs[i * 4 + 3]};
    if (i + 1 >= s.inputs.size() || s.interpolation == AnimationInterpolation::Step) {
        return q0;
    }
    const Quat q1{s.outputs[(i + 1) * 4 + 0], s.outputs[(i + 1) * 4 + 1], s.outputs[(i + 1) * 4 + 2], s.outputs[(i + 1) * 4 + 3]};
    const float t0 = s.inputs[i];
    const float t1 = s.inputs[i + 1];
    const float a = (t - t0) / (t1 - t0);
    return Quat::slerp(q0, q1, a);
}

}  // namespace

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
    const std::size_t n = std::min(out.size(), skeleton_->bones.size());

    // Step 1: per-bone TRS, defaulting to bind-pose decomposition.
    std::vector<Vec3> trans(n);
    std::vector<Quat> rot(n);
    std::vector<Vec3> scale(n);
    for (std::size_t i = 0; i < n; ++i) {
        decomposeTRS(skeleton_->bones[i].localBindTransform, trans[i], rot[i], scale[i]);
    }

    // Step 2: overlay clip channels.
    if (clip_) {
        for (const auto& ch : clip_->channels) {
            if (ch.targetBone < 0 || static_cast<std::size_t>(ch.targetBone) >= n) continue;
            const AnimationSampler& s = clip_->samplers[ch.samplerIndex];
            switch (ch.path) {
                case AnimationPath::Translation: trans[ch.targetBone] = sampleVec3(s, time_); break;
                case AnimationPath::Rotation:    rot[ch.targetBone]   = sampleQuat(s, time_); break;
                case AnimationPath::Scale:       scale[ch.targetBone] = sampleVec3(s, time_); break;
            }
        }
    }

    // Step 3: compose local matrices and walk hierarchy.
    // Bones are stored such that any bone's parent has a lower index
    // (glTF best practice; M23 loader emits them via DFS so this holds).
    std::vector<Mat4> globals(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4 localMat = composeTRS(trans[i], rot[i], scale[i]);
        const int parent = skeleton_->bones[i].parentIndex;
        if (parent < 0) {
            globals[i] = localMat;
        } else {
            assert(static_cast<std::size_t>(parent) < i && "bone parent must have lower index");
            globals[i] = globals[parent] * localMat;
        }
        out[i] = globals[i] * skeleton_->bones[i].inverseBindMatrix;
    }
}

}  // namespace iron
```

- [ ] **Step 6: Run tests to verify they pass**

```powershell
cmake --build build-vk --target test_animation_player --config Debug
ctest --test-dir build-vk -R animation_player --output-on-failure
```

Expected: all 6 `AnimationPlayer_*` tests pass.

- [ ] **Step 7: Run the full test suite to confirm no regressions**

```powershell
ctest --test-dir build-vk --output-on-failure
```

Expected: every existing test still passes (skinning, gltf_loader, quaternion, etc.).

- [ ] **Step 8: Commit**

```bash
git add engine/asset/AnimationPlayer.h engine/asset/AnimationPlayer.cpp tests/test_animation_player.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M24 Task 3: AnimationPlayer with TRS sampling and hierarchy walk"
```

---

## Task 4: glTF animation loader

**Files:**
- Modify: `engine/asset/GltfLoader.h`
- Modify: `engine/asset/GltfLoader.cpp`
- Modify: `tests/test_gltf_loader.cpp`

**Background you need to know:**
- M23's `GltfLoader.cpp` already builds a node-index → bone-index map while constructing the `Skeleton`. Find that map (likely a local `std::unordered_map<int, int>` named something like `nodeToBone`). You will reuse it to resolve `channel.target.node` → bone index.
- tinygltf exposes `model.animations[a].samplers[s]` with fields `input` (accessor index for time), `output` (accessor index for values), `interpolation` (string: "LINEAR"/"STEP"/"CUBICSPLINE").
- `model.animations[a].channels[c]` has `sampler` (index into samplers[]) and `target.node` (int) + `target.path` (string: "translation"/"rotation"/"scale"/"weights").
- Accessor reading: M23 already has helpers `readVec3FloatAccessor`, `readVec4FloatAccessor`, and a scalar float reader. Reuse those. If a scalar reader doesn't exist, add an inline one using the same pattern (read `accessor.componentType`, assert FLOAT, copy `count * 1` floats out of the buffer view at the right offset).

- [ ] **Step 1: Extend GltfModel in `engine/asset/GltfLoader.h`**

Replace the existing struct:

```cpp
#include "asset/Animation.h"
// ...
struct GltfModel {
    MeshData                       mesh;
    GltfMaterialPaths              materialPaths;
    std::optional<SkinnedMeshData> skinnedMesh;
    std::vector<AnimationClip>     animations;  // empty if file has no animations
};
```

- [ ] **Step 2: Write a failing test in `tests/test_gltf_loader.cpp`**

Append:

```cpp
TEST(GltfLoader_RiggedSimple_LoadsOneAnimation) {
    const auto model = iron::loadGltfModel("tests/assets/gltf/RiggedSimple.gltf");
    ASSERT_TRUE(model.has_value());
    ASSERT_EQ(model->animations.size(), 1u);
    const auto& clip = model->animations[0];
    EXPECT_GT(clip.duration, 0.0f);
    EXPECT_FALSE(clip.channels.empty());
    EXPECT_FALSE(clip.samplers.empty());
    // RiggedSimple animates one bone rotating, so at least one channel should
    // resolve to a valid bone index and have path == Rotation.
    bool foundRot = false;
    for (const auto& ch : clip.channels) {
        if (ch.targetBone >= 0 && ch.path == iron::AnimationPath::Rotation) {
            foundRot = true;
            break;
        }
    }
    EXPECT_TRUE(foundRot);
}
```

- [ ] **Step 3: Run test to verify it fails**

```powershell
cmake --build build-vk --target test_gltf_loader --config Debug
ctest --test-dir build-vk -R gltf_loader -R RiggedSimple_LoadsOneAnimation --output-on-failure
```

Expected: compile error (`animations` member missing) until Step 1 is in, then test fails because the loader doesn't populate `animations` yet.

- [ ] **Step 4: Implement animation parsing in `engine/asset/GltfLoader.cpp`**

After the skin/skeleton section (where `model.animations` is currently ignored), add:

```cpp
// --- M24: animations -------------------------------------------------
// nodeToBone is the node-index -> bone-index map built during skeleton
// load above. If the local variable is named differently, rename uses
// below to match.
static bool warnedCubicSpline = false;
static bool warnedWeightsPath = false;

for (const auto& gltfAnim : model.animations) {
    AnimationClip clip;
    clip.name = gltfAnim.name;

    // Load samplers first so channels can reference them by index.
    clip.samplers.reserve(gltfAnim.samplers.size());
    for (const auto& gs : gltfAnim.samplers) {
        AnimationSampler samp;

        // Decode interpolation; downgrade CubicSpline to Linear with one warn.
        if (gs.interpolation == "STEP") {
            samp.interpolation = AnimationInterpolation::Step;
        } else if (gs.interpolation == "CUBICSPLINE") {
            samp.interpolation = AnimationInterpolation::Linear;
            if (!warnedCubicSpline) {
                LogWarn("glTF CUBICSPLINE interpolation not supported; downgrading to LINEAR");
                warnedCubicSpline = true;
            }
        } else {
            samp.interpolation = AnimationInterpolation::Linear;
        }

        // Inputs: scalar floats (keyframe times in seconds).
        if (!readScalarFloatAccessor(model, gs.input, samp.inputs)) {
            LogWarn("Animation sampler has unreadable input accessor; skipping");
            continue;
        }
        // Outputs: 3 or 4 floats per keyframe depending on the channel's
        // path, but we don't know the path here. Read raw floats; channels
        // will use them according to their own path.
        if (!readScalarFloatAccessor(model, gs.output, samp.outputs)) {
            LogWarn("Animation sampler has unreadable output accessor; skipping");
            continue;
        }
        clip.samplers.push_back(std::move(samp));
    }

    // Channels: bind sampler -> (bone, path).
    clip.channels.reserve(gltfAnim.channels.size());
    for (const auto& gc : gltfAnim.channels) {
        AnimationChannel ch;
        ch.samplerIndex = gc.sampler;

        const std::string& path = gc.target_path;
        if (path == "translation") {
            ch.path = AnimationPath::Translation;
        } else if (path == "rotation") {
            ch.path = AnimationPath::Rotation;
        } else if (path == "scale") {
            ch.path = AnimationPath::Scale;
        } else {
            // "weights" (morph targets) — skip.
            if (!warnedWeightsPath) {
                LogWarn("glTF animation 'weights' path not supported; skipping channel");
                warnedWeightsPath = true;
            }
            continue;
        }

        auto it = nodeToBone.find(gc.target_node);
        ch.targetBone = (it != nodeToBone.end()) ? it->second : -1;
        clip.channels.push_back(ch);
    }

    // Duration = max over all samplers' input.back().
    clip.duration = 0.0f;
    for (const auto& samp : clip.samplers) {
        if (!samp.inputs.empty()) {
            clip.duration = std::max(clip.duration, samp.inputs.back());
        }
    }

    out.animations.push_back(std::move(clip));
}
```

If `readScalarFloatAccessor` doesn't already exist next to `readVec3FloatAccessor` / `readVec4FloatAccessor`, add it in the same anonymous-namespace block at the top of the file. The implementation pattern is identical to the existing readers — just `count * 1` floats instead of `count * 3` or `count * 4`. Stub:

```cpp
bool readScalarFloatAccessor(const tinygltf::Model& m, int accessorIndex, std::vector<float>& out) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(m.accessors.size())) return false;
    const auto& a = m.accessors[accessorIndex];
    if (a.type != TINYGLTF_TYPE_SCALAR) return false;
    if (a.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
    const auto& bv = m.bufferViews[a.bufferView];
    const auto& buf = m.buffers[bv.buffer];
    const std::size_t stride = bv.byteStride ? bv.byteStride : sizeof(float);
    const std::uint8_t* base = buf.data.data() + bv.byteOffset + a.byteOffset;
    out.resize(a.count);
    for (std::size_t i = 0; i < a.count; ++i) {
        std::memcpy(&out[i], base + i * stride, sizeof(float));
    }
    return true;
}
```

(Verify against the existing readers' style — if they take the buffer differently, mirror that.)

- [ ] **Step 5: Run test to verify it passes**

```powershell
cmake --build build-vk --target test_gltf_loader --config Debug
ctest --test-dir build-vk -R gltf_loader --output-on-failure
```

Expected: `GltfLoader_RiggedSimple_LoadsOneAnimation` passes, existing gltf_loader tests still pass.

- [ ] **Step 6: Full test suite**

```powershell
ctest --test-dir build-vk --output-on-failure
```

Expected: everything green.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/GltfLoader.h engine/asset/GltfLoader.cpp tests/test_gltf_loader.cpp
git commit -m "M24 Task 4: parse glTF animations into AnimationClip array"
```

---

## Task 5: Wire AnimationPlayer into 10-gltf-viewer

**Files:**
- Modify: `games/10-gltf-viewer/main.cpp`

- [ ] **Step 1: Add the include and a frame-time helper**

Near the top with the other engine includes:

```cpp
#include "asset/AnimationPlayer.h"
```

- [ ] **Step 2: Replace the static bind-pose fill with the player**

Find the per-frame block that currently fills `bonesPose` with `iron::Mat4::identity()` (around line 441-445 of the M23 main.cpp). Replace the identity-fill with the player evaluate. Concretely:

Before the main loop, after `skinnedMesh = renderer.createSkinnedMesh(*model->skinnedMesh);`:

```cpp
iron::AnimationPlayer animPlayer;
if (isSkinned) {
    animPlayer.setSkeleton(&model->skinnedMesh->skeleton);
    if (!model->animations.empty()) {
        animPlayer.setClip(&model->animations[0]);
        iron::LogInfo("Playing animation '%s' (duration %.2fs, %zu channels)",
                      model->animations[0].name.c_str(),
                      model->animations[0].duration,
                      model->animations[0].channels.size());
    } else {
        iron::LogInfo("Model has skeleton but no animations; showing bind pose");
    }
}
```

In the per-frame block, just before constructing `iron::SkinnedDrawCall`, replace:

```cpp
std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bonesPose;
for (auto& m : bonesPose) m = iron::Mat4::identity();
const std::size_t boneCount = model->skinnedMesh->skeleton.bones.size();
```

with:

```cpp
std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bonesPose;
for (auto& m : bonesPose) m = iron::Mat4::identity();
const std::size_t boneCount = model->skinnedMesh->skeleton.bones.size();
animPlayer.update(dt);  // `dt` is the existing per-frame delta in this loop
animPlayer.evaluate(std::span<iron::Mat4>(bonesPose.data(), std::min(boneCount, bonesPose.size())));
```

If the surrounding loop's frame-delta variable is not named `dt`, look near the top of the loop for `glfwGetTime` / `lastTime` and use that name. The existing FreeFlyCamera update should already be using it — grep for `camera.update(` to find the local name.

- [ ] **Step 3: Build the viewer**

```powershell
cmake --build build-vk --target 10-gltf-viewer --config Debug
```

Expected: clean build, no warnings about unused variables.

- [ ] **Step 4: Run the viewer and visually confirm**

```powershell
.\build-vk\games\10-gltf-viewer\Debug\10-gltf-viewer.exe --model rigged-simple
```

Expected:
- Console prints "Playing animation '...' (duration ...s, 1 channels)" (RiggedSimple has 1 channel — one bone rotating).
- The cylinder visibly bends/rotates over a ~1.25s loop.
- No flicker, no NaN-stretched triangles, no crash.

If the cylinder is static, animPlayer.evaluate isn't running — verify `isSkinned` is true and `model->animations.empty()` is false.

- [ ] **Step 5: Commit**

```bash
git add games/10-gltf-viewer/main.cpp
git commit -m "M24 Task 5: drive RiggedSimple bone palette from AnimationPlayer"
```

---

## Task 6: Docs + PR

**Files:**
- Modify: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Append an M24 section to `docs/engine/asset-pipeline.md`**

Match the surrounding style (the M22, M22.5, M23 sections set the template). Cover:
- What got added (`Animation.h`, `AnimationPlayer.h/.cpp`, GltfLoader extension).
- The runtime data flow: glTF file → `loadGltfModel` → `GltfModel::animations` → `AnimationPlayer.setClip` → `evaluate(palette)` → `SkinnedDrawCall.boneMatrices`.
- Limitations: one clip at a time per player; no blending; CubicSpline downgraded to Linear; morph-target "weights" channels dropped.
- The verification command:
  ```powershell
  .\build-vk\games\10-gltf-viewer\Debug\10-gltf-viewer.exe --model rigged-simple
  ```

- [ ] **Step 2: Commit and push**

```bash
git add docs/engine/asset-pipeline.md
git commit -m "M24: document animation playback in asset-pipeline.md"
git push -u origin feat/m24-animation-playback
```

- [ ] **Step 3: Open the PR**

```powershell
gh pr create --title "M24: glTF animation playback" --body "$(cat docs/superpowers/plans/2026-05-28-m24-animation-playback.md)"
```

Adjust the body to a summary form if the full plan is too long.

---

## Verification Checklist (run before declaring done)

- [ ] `ctest --test-dir build-vk --output-on-failure` — full suite green
- [ ] `10-gltf-viewer --model rigged-simple` shows visibly animating mesh
- [ ] `10-gltf-viewer --model damaged-helmet` still works (regression check — non-skinned path unchanged)
- [ ] No new warnings during cmake configure or build
- [ ] PR CI green
