# M25: Skinned Characters in net-shooter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-player colored cubes in `games/07-net-shooter` with a skinned Fox mesh whose animation clip switches between Survey (idle), Walk, and Run based on each player's movement state. Closes the skeletal-animation track (M22 → M22.5 → M23 → M24 → M25).

**Architecture:** Add a small `findClip` lookup helper on `GltfModel`. Introduce `iron::CharacterAnimator` — an engine-level wrapper around `AnimationPlayer` with a state-name → clip map and idempotent hard-cut switching. In net-shooter, load Fox.glb once, build per-player `CharacterAnimator` instances lazily on first sight, derive movement state each frame from position deltas + grounded flag, and submit a `SkinnedDrawCall` instead of the cube. M21 ragdoll handoff is preserved by the existing skip-on-active-ragdoll guard.

**Tech Stack:** C++23 (`/std:c++latest`), Vulkan 1.3, glTF (Fox.glb from Khronos CC0 samples), CMake. Builds on M23 (`SkinnedMeshHandle`, `SkinnedDrawCall`, `kMaxBonesPerSkinnedMesh`) and M24 (`AnimationClip`, `AnimationPlayer`, `GltfModel::animations`).

**Spec:** `docs/superpowers/specs/2026-05-28-m25-skinned-characters-net-shooter-design.md`

**Prerequisite:** M24 PR #44 is merged to main (verified — commit `c7f205e`). Branch this work from `main`; branch already exists as `feat/m25-skinned-characters` with the spec doc committed.

---

## File Structure

**Create:**
- `engine/asset/CharacterAnimator.h` — declaration of `iron::CharacterAnimator`.
- `engine/asset/CharacterAnimator.cpp` — state-map + switch semantics, thin wrappers around `AnimationPlayer`.
- `tests/test_character_animator.cpp` — unit tests for state-switch + null-clip safety + idempotent re-switch.
- `games/07-net-shooter/assets/fox/Fox.glb` — Khronos CC0 sample asset.

**Modify:**
- `engine/asset/GltfLoader.h` — add `const AnimationClip* findClip(std::string_view name) const` to `GltfModel`.
- `engine/asset/GltfLoader.cpp` — implement `findClip` (linear scan).
- `engine/CMakeLists.txt` — add `engine/asset/CharacterAnimator.cpp` to the `ironcore` target.
- `tests/CMakeLists.txt` — register `test_character_animator`.
- `tests/test_gltf_loader.cpp` — small test that `findClip("Survey")` finds the RiggedSimple clip (re-using M24 test data with a clip name lookup).
- `games/07-net-shooter/main.cpp` — load fox; build per-player animators; replace `submitPlayerCube` calls with skinned submission; track previous positions for speed derivation; add skinned vertex shader source string.
- `games/07-net-shooter/CMakeLists.txt` — copy `games/07-net-shooter/assets/` to the build output (additional to the existing global-assets copy).
- `docs/engine/asset-pipeline.md` — append M25 section.

**Out of scope (explicit non-goals per the spec):**
- Crossfading / blending between clips — M26+.
- Per-character selection — game-level concern.
- Network sync of animation state — derived locally from synced position.
- Shoot/death/hit animations — coupled with weapon/M21 systems; deferred.
- Visible rendering of the local player — first-person, camera is at player position; remote-only continues.

---

## Task 1: `GltfModel::findClip` helper

**Files:**
- Modify: `engine/asset/GltfLoader.h`
- Modify: `engine/asset/GltfLoader.cpp`
- Modify: `tests/test_gltf_loader.cpp`

- [ ] **Step 1: Add a failing test in `tests/test_gltf_loader.cpp`**

Append to the file in the same harness style M24 used (`CHECK(...)` macros — verify by reading nearby tests). Use the existing RiggedSimple.gltf which M23 vendored under `tests/assets/gltf/`:

```cpp
TEST(GltfLoader_FindClipByName) {
    const auto model = iron::loadGltfModel("tests/assets/gltf/RiggedSimple.gltf");
    REQUIRE(model.has_value());
    REQUIRE_FALSE(model->animations.empty());
    const std::string& name = model->animations[0].name;
    // RiggedSimple's animation has an empty name in the glTF; we still want
    // exact-match lookup to work even on empty string.
    const iron::AnimationClip* found = model->findClip(name);
    CHECK(found == &model->animations[0]);
    CHECK(model->findClip("does-not-exist") == nullptr);
}
```

(Adjust the `TEST` / `CHECK` / `REQUIRE` macro names to whatever `tests/test_gltf_loader.cpp` actually uses — read the top of that file first. If `REQUIRE_FALSE` is `CHECK(!...)`, use the local equivalent.)

- [ ] **Step 2: Run test to verify it fails**

```powershell
cmake --build build-vk --target test_gltf_loader --config Debug
```

Expected: compile error referencing `findClip` (method does not exist).

- [ ] **Step 3: Add the declaration in `engine/asset/GltfLoader.h`**

Inside `struct GltfModel`, immediately after the `std::vector<AnimationClip> animations;` field (M24 added that):

```cpp
    // Returns a pointer to the first clip whose name matches `name`,
    // or nullptr if no match. Linear scan; clip counts are tiny in v1.
    const AnimationClip* findClip(std::string_view name) const;
```

- [ ] **Step 4: Implement in `engine/asset/GltfLoader.cpp`**

Add at the very end of the file (still inside `namespace iron`, before the closing brace):

```cpp
const AnimationClip* GltfModel::findClip(std::string_view name) const {
    for (const auto& clip : animations) {
        if (clip.name == name) return &clip;
    }
    return nullptr;
}
```

- [ ] **Step 5: Run test to verify it passes + full suite**

```powershell
cmake --build build-vk --target test_gltf_loader --config Debug
ctest --test-dir build-vk --output-on-failure
```

Expected: `test_gltf_loader` passes including the new `FindClipByName` case; full suite green (39/39 from M24 baseline, now 40/40 or +1 in the gltf_loader binary).

- [ ] **Step 6: Commit**

```bash
git add engine/asset/GltfLoader.h engine/asset/GltfLoader.cpp tests/test_gltf_loader.cpp
git commit -m "M25 Task 1: add GltfModel::findClip name-lookup helper"
```

---

## Task 2: `CharacterAnimator` class (TDD)

**Files:**
- Create: `engine/asset/CharacterAnimator.h`
- Create: `engine/asset/CharacterAnimator.cpp`
- Create: `tests/test_character_animator.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Declare `CharacterAnimator` in `engine/asset/CharacterAnimator.h`**

```cpp
#pragma once

#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {

// Drives one skinned character. Owns no glTF data: the Skeleton and the
// AnimationClips remain owned by the GltfModel that loaded them. The
// animator just keeps non-owning pointers and a state-name -> clip map.
//
// Typical use:
//   CharacterAnimator a;
//   a.setSkeleton(&model.skinnedMesh->skeleton);
//   a.setClipForState("idle", model.findClip("Survey"));
//   a.setClipForState("walk", model.findClip("Walk"));
//   a.setClipForState("run",  model.findClip("Run"));
//   // each frame:
//   a.switchTo("run");   // no-op if already in "run"; hard cut otherwise
//   a.update(dt);
//   a.evaluate(boneMatrices);
class CharacterAnimator {
public:
    // Bind the skeleton this animator drives. Non-owning; the model must
    // outlive the animator. Resets the inner AnimationPlayer.
    void setSkeleton(const Skeleton* skeleton);

    // Register a clip under a string state name. A null clip is legal and
    // means "this state has no animation; evaluate() writes the bind pose".
    void setClipForState(std::string state, const AnimationClip* clip);

    // Switch to a named state. If we're already in `state`, this is a no-op
    // (the active clip keeps advancing). On a real change, the clip time
    // resets to zero (hard cut). If `state` was never registered, logs
    // once and falls back to the bind pose.
    void switchTo(std::string_view state);

    // Advance the active clip by dt seconds.
    void update(float dt);

    // Write the bone-matrix palette via the inner AnimationPlayer.
    void evaluate(std::span<Mat4> out) const;

    std::string_view currentState() const { return currentState_; }

private:
    AnimationPlayer                                       player_;
    std::unordered_map<std::string, const AnimationClip*> clips_;
    std::string                                           currentState_;
    bool                                                  warnedUnknownState_ = false;
};

}  // namespace iron
```

- [ ] **Step 2: Write failing tests in `tests/test_character_animator.cpp`**

Adapt macro names to the project harness (look at `tests/test_animation_player.cpp` for the conventions M24 settled on):

```cpp
#include "asset/Animation.h"
#include "asset/AnimationPlayer.h"
#include "asset/CharacterAnimator.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <array>
#include <cmath>

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

// Clip that translates bone 0 from x=0 at t=0 to x=10 at t=1.
AnimationClip makeTranslateClip(const char* name) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.0f;
    AnimationSampler s;
    s.inputs  = {0.0f, 1.0f};
    s.outputs = {0,0,0,  10,0,0};
    s.interpolation = AnimationInterpolation::Linear;
    c.samplers.push_back(s);
    AnimationChannel ch{0, AnimationPath::Translation, 0};
    c.channels.push_back(ch);
    return c;
}

}  // namespace

TEST(CharacterAnimator_SwitchToRegisteredStatePlaysClip) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip walk = makeTranslateClip("Walk");
    CharacterAnimator a;
    a.setSkeleton(&sk);
    a.setClipForState("walk", &walk);
    a.switchTo("walk");
    a.update(0.5f);
    std::array<Mat4, 1> out{};
    a.evaluate(out);
    // Halfway through translation: x = 5.
    CHECK_NEAR(out[0].at(0, 3), 5.0f, 1e-4f);
}

TEST(CharacterAnimator_SwitchToSameStateIsIdempotent) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip walk = makeTranslateClip("Walk");
    CharacterAnimator a;
    a.setSkeleton(&sk);
    a.setClipForState("walk", &walk);
    a.switchTo("walk");
    a.update(0.5f);   // time = 0.5
    a.switchTo("walk"); // SAME state; time must NOT reset
    a.update(0.25f);  // time = 0.75
    std::array<Mat4, 1> out{};
    a.evaluate(out);
    CHECK_NEAR(out[0].at(0, 3), 7.5f, 1e-4f);
}

TEST(CharacterAnimator_SwitchToDifferentStateResetsTime) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip walk = makeTranslateClip("Walk");
    const AnimationClip run  = makeTranslateClip("Run");
    CharacterAnimator a;
    a.setSkeleton(&sk);
    a.setClipForState("walk", &walk);
    a.setClipForState("run",  &run);
    a.switchTo("walk");
    a.update(0.5f);    // walk time = 0.5
    a.switchTo("run"); // DIFFERENT state; clip time resets to 0
    std::array<Mat4, 1> out{};
    a.evaluate(out);
    // Time = 0 on the run clip -> x = 0.
    CHECK_NEAR(out[0].at(0, 3), 0.0f, 1e-4f);
}

TEST(CharacterAnimator_NullClipStateProducesBindPose) {
    const Skeleton sk = makeTrivialSkeleton();
    CharacterAnimator a;
    a.setSkeleton(&sk);
    a.setClipForState("idle", nullptr);  // explicitly null
    a.switchTo("idle");
    a.update(1.0f);
    std::array<Mat4, 1> out{};
    a.evaluate(out);
    // Bind pose = identity for the trivial skeleton.
    CHECK_NEAR(out[0].at(0, 0), 1.0f, 1e-5f);
    CHECK_NEAR(out[0].at(0, 3), 0.0f, 1e-5f);
}

TEST(CharacterAnimator_UnknownStateFallsBackToBindPose) {
    const Skeleton sk = makeTrivialSkeleton();
    const AnimationClip walk = makeTranslateClip("Walk");
    CharacterAnimator a;
    a.setSkeleton(&sk);
    a.setClipForState("walk", &walk);
    a.switchTo("jump");  // never registered
    a.update(1.0f);
    std::array<Mat4, 1> out{};
    a.evaluate(out);
    // Bind pose for the trivial skeleton.
    CHECK_NEAR(out[0].at(0, 0), 1.0f, 1e-5f);
    CHECK_NEAR(out[0].at(0, 3), 0.0f, 1e-5f);
}
```

- [ ] **Step 3: Wire up CMake**

In `engine/CMakeLists.txt`, find the line that adds `engine/asset/AnimationPlayer.cpp` (added in M24) and add `engine/asset/CharacterAnimator.cpp` directly after it. Match surrounding style.

In `tests/CMakeLists.txt`, find the line registering `test_animation_player` (added in M24). Add `test_character_animator` next to it, mirroring the same incantation (likely `iron_add_test(test_character_animator)`).

- [ ] **Step 4: Run tests to confirm they fail**

```powershell
cmake --build build-vk --target test_character_animator --config Debug
ctest --test-dir build-vk -R character_animator --output-on-failure
```

Expected: link error (CharacterAnimator symbols unresolved) before Step 5.

- [ ] **Step 5: Implement `engine/asset/CharacterAnimator.cpp`**

```cpp
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
```

NOTE: Verify `Log::warn` accepts a `%.*s` printf-style format. M24 confirmed `Log::warn` is the right name and that it takes printf format strings. If it doesn't accept `%.*s`, fall back to constructing a `std::string(state)` and passing `%s` with `.c_str()` — slightly less efficient but works.

- [ ] **Step 6: Run tests to verify they pass + full suite**

```powershell
cmake --build build-vk --target test_character_animator --config Debug
ctest --test-dir build-vk --output-on-failure
```

Expected: all 5 `CharacterAnimator_*` tests pass; full suite green; no regressions.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/CharacterAnimator.h engine/asset/CharacterAnimator.cpp tests/test_character_animator.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M25 Task 2: CharacterAnimator with state-name clip switching"
```

---

## Task 3: Vendor Fox.glb + load smoke test

**Files:**
- Create: `games/07-net-shooter/assets/fox/Fox.glb`
- Modify: `tests/test_gltf_loader.cpp` (small smoke check)

- [ ] **Step 1: Download Fox.glb from the Khronos glTF-Sample-Assets repo**

The Khronos repository keeps Fox at this raw URL (verify the path before download — repo structure occasionally moves):

```
https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Fox/glTF-Binary/Fox.glb
```

Use PowerShell:

```powershell
$dest = 'C:\Users\elias\Documents\_dev\iron-core-engine\games\07-net-shooter\assets\fox'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$src  = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Fox/glTF-Binary/Fox.glb'
Invoke-WebRequest -Uri $src -OutFile (Join-Path $dest 'Fox.glb')
```

If the URL 404s (Khronos restructured the repo), use the v1 fallback:

```
https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/master/2.0/Fox/glTF-Binary/Fox.glb
```

Verify the file is non-empty (~400 KB):

```powershell
Get-Item (Join-Path $dest 'Fox.glb') | Select-Object Length
```

Expected: `Length` ~ 400000 bytes.

- [ ] **Step 2: Add a smoke-load test to `tests/test_gltf_loader.cpp`**

Vendor a copy of Fox.glb under `tests/assets/gltf/` too so the test doesn't depend on the game's asset directory:

```powershell
Copy-Item 'C:\Users\elias\Documents\_dev\iron-core-engine\games\07-net-shooter\assets\fox\Fox.glb' `
          'C:\Users\elias\Documents\_dev\iron-core-engine\tests\assets\gltf\Fox.glb'
```

Append the test (use the actual harness macro names):

```cpp
TEST(GltfLoader_Fox_LoadsSkinAndThreeNamedClips) {
    const auto model = iron::loadGltfModel("tests/assets/gltf/Fox.glb");
    REQUIRE(model.has_value());
    REQUIRE(model->skinnedMesh.has_value());
    CHECK(model->skinnedMesh->skeleton.bones.size() > 0);
    CHECK(model->skinnedMesh->skeleton.bones.size() <= iron::kMaxBonesPerSkinnedMesh);

    CHECK(model->findClip("Survey") != nullptr);
    CHECK(model->findClip("Walk")   != nullptr);
    CHECK(model->findClip("Run")    != nullptr);
}
```

- [ ] **Step 3: Run the smoke test**

```powershell
cmake --build build-vk --target test_gltf_loader --config Debug
ctest --test-dir build-vk -R gltf_loader --output-on-failure
```

Expected: test passes. If it doesn't, the Fox.glb file may have moved or the clip names may differ — log the actual `animations` vector content from `loadGltfModel` and update the test (the names `Survey`, `Walk`, `Run` are stable in the published Khronos sample at time of writing).

- [ ] **Step 4: Commit**

```bash
git add games/07-net-shooter/assets/fox/Fox.glb tests/assets/gltf/Fox.glb tests/test_gltf_loader.cpp
git commit -m "M25 Task 3: vendor Fox.glb (Khronos CC0) + smoke-load test"
```

---

## Task 4: Wire net-shooter to render skinned foxes

**Files:**
- Modify: `games/07-net-shooter/main.cpp`
- Modify: `games/07-net-shooter/CMakeLists.txt`

This is the largest task. Break it into substeps; commit after the full wire-up compiles cleanly even if it's not yet visually correct.

- [ ] **Step 1: Extend the CMakeLists to copy the local `assets/` directory**

Append to `games/07-net-shooter/CMakeLists.txt`:

```cmake
# M25 — copy Fox.glb (and any future per-game assets) next to the exe.
add_custom_command(TARGET net-shooter POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-shooter>/assets
  COMMENT "Copying net-shooter local assets")
```

This runs in addition to the existing global-assets copy. Build to confirm it works:

```powershell
cmake --build build-vk --target net-shooter --config Debug
```

Expected: clean build; `build-vk/games/07-net-shooter/Debug/assets/fox/Fox.glb` exists post-build.

- [ ] **Step 2: Add the skinned vertex shader source and engine includes**

Near the top of `games/07-net-shooter/main.cpp`, add these includes (next to the existing `#include "render/Renderer.h"`):

```cpp
#include "asset/CharacterAnimator.h"
#include "asset/GltfLoader.h"
```

Add the skinned vertex shader string near `kVertexShader` (copy verbatim from `games/10-gltf-viewer/main.cpp` — search for `kSkinnedVertexShader` in that file). For maintainability, copy the comment block above it too. The fragment shader is reused unchanged.

- [ ] **Step 3: Load fox + create skinned mesh and shader once at startup**

After the existing `litShader` creation block (around the area where the cube/ground meshes are created — search for `cubeMesh = renderer.createMesh(cubeData);` for the anchor), add:

```cpp
// --- M25 ---------------------------------------------------------------
// Load Fox.glb once; build skinned mesh + skinned shader; cache the three
// named animation clips by pointer.
const auto foxModelOpt = iron::loadGltfModel("assets/fox/Fox.glb");
if (!foxModelOpt || !foxModelOpt->skinnedMesh) {
    iron::Log::warn("net-shooter: Fox.glb missing or has no skin; players "
                    "will render as cubes (M22.5 fallback)");
}
const iron::GltfModel* foxModel = foxModelOpt ? &*foxModelOpt : nullptr;
const iron::SkinnedMeshHandle foxMesh =
    (foxModel && foxModel->skinnedMesh)
        ? renderer.createSkinnedMesh(*foxModel->skinnedMesh)
        : iron::kInvalidSkinnedMesh;
const iron::ShaderHandle foxShader =
    (foxMesh != iron::kInvalidSkinnedMesh)
        ? renderer.createSkinnedShader(kSkinnedVertexShader, kFragmentShader)
        : iron::kInvalidHandle;

const iron::AnimationClip* foxIdleClip =
    foxModel ? foxModel->findClip("Survey") : nullptr;
const iron::AnimationClip* foxWalkClip =
    foxModel ? foxModel->findClip("Walk")   : nullptr;
const iron::AnimationClip* foxRunClip  =
    foxModel ? foxModel->findClip("Run")    : nullptr;

const bool foxReady = foxMesh != iron::kInvalidSkinnedMesh &&
                      foxShader != iron::kInvalidHandle &&
                      foxIdleClip && foxWalkClip && foxRunClip;
if (!foxReady) {
    iron::Log::warn("net-shooter: fox skinned path not ready; falling back "
                    "to cube rendering for players");
}
```

Note that the fallback path is deliberate — if anything's wrong with the asset, the game still runs (just shows cubes again). Avoids gating playability on the new path.

- [ ] **Step 4: Add per-player animator + previous-position maps**

In the same scope as `hostPlayers`, `authStates`, etc. (search for `std::unordered_map<std::uint32_t, ` to find existing similar maps), add:

```cpp
// M25 — per-peer skinned-character state (lazy: created on first sight).
std::unordered_map<std::uint32_t, iron::CharacterAnimator> playerAnimators;
std::unordered_map<std::uint32_t, iron::Vec3>             playerPrevPos;
```

- [ ] **Step 5: Replace the `submitPlayerCube` lambda with a skinned variant (cube fallback preserved)**

Find the `auto submitPlayerCube = [&](std::uint32_t pid, const iron::Vec3& pos)` lambda (around line 1931 of main.cpp on this branch). Replace it with:

```cpp
auto submitPlayerCube = [&](std::uint32_t pid, const iron::Vec3& pos) {
    const iron::Vec3 halfE = iron::netshooter::kPlayerHalfExtents;
    iron::DrawCall call;
    call.mesh = cubeMesh;
    call.shader = litShader;
    call.model = iron::translation(iron::Vec3{pos.x, pos.y + halfE.y, pos.z})
               * iron::scaling(iron::Vec3{halfE.x*2, halfE.y*2, halfE.z*2});
    call.material.texture     = renderer.whiteTexture();
    call.material.normalMap   = renderer.flatNormalTexture();
    call.material.specularMap = renderer.noSpecularTexture();
    call.material.emissive    = colorForPeer(pid) * 0.5f;
    renderer.submit(call);
};

// M25 — submit a skinned fox at world position `pos` for peer `pid`. The
// per-player CharacterAnimator is created lazily on first sight. State
// selection (idle / walk / run) is derived from this frame's speed and the
// caller-provided `grounded` flag.
auto submitPlayerFox = [&](std::uint32_t pid, const iron::Vec3& pos,
                            bool grounded, float dt) {
    auto [animIt, inserted] = playerAnimators.try_emplace(pid);
    if (inserted) {
        animIt->second.setSkeleton(&foxModel->skinnedMesh->skeleton);
        animIt->second.setClipForState("idle", foxIdleClip);
        animIt->second.setClipForState("walk", foxWalkClip);
        animIt->second.setClipForState("run",  foxRunClip);
        playerPrevPos[pid] = pos;  // first sample; no speed this frame
    }

    const iron::Vec3 prev = playerPrevPos[pid];
    const float speed = (dt > 1e-6f)
        ? iron::length(iron::Vec3{pos.x - prev.x, pos.y - prev.y, pos.z - prev.z}) / dt
        : 0.0f;
    playerPrevPos[pid] = pos;

    const char* state =
          !grounded         ? "walk"
        : speed < 0.5f      ? "idle"
        : speed < 3.5f      ? "walk"
        :                     "run";
    animIt->second.switchTo(state);
    animIt->second.update(dt);

    std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bones;
    for (auto& m : bones) m = iron::Mat4::identity();
    const std::size_t n = foxModel->skinnedMesh->skeleton.bones.size();
    animIt->second.evaluate(std::span<iron::Mat4>(bones.data(),
                                                  std::min(n, bones.size())));

    iron::SkinnedDrawCall call;
    call.skinnedMesh  = foxMesh;
    call.shader       = foxShader;
    // The fox model's local origin sits at floor height — translate to
    // the player's foot position (pos.y is already foot-aligned per
    // the existing cube convention).
    call.model        = iron::translation(pos);
    call.material.texture     = renderer.whiteTexture();
    call.material.normalMap   = renderer.flatNormalTexture();
    call.material.specularMap = renderer.noSpecularTexture();
    call.material.emissive    = colorForPeer(pid) * 0.3f;
    call.boneMatrices = std::span<const iron::Mat4>(bones.data(), n);
    renderer.submitSkinnedDraw(call);
};
```

Note: `iron::length(Vec3)` is the free function used in M24's `AnimationPlayer.cpp`; confirm it exists in `engine/math/Vec.h`.

- [ ] **Step 6: Call `submitPlayerFox` from both host and client render branches**

Find the two existing call sites (around line 1956 and 1968 — both currently call `submitPlayerCube`). Replace each with:

```cpp
// HOST branch (around line 1956):
const bool grounded =
    (hostPlayers.find(pid) != hostPlayers.end())
        ? hostPlayers[pid].grounded
        : true;  // unknown -> assume grounded; renders idle/walk/run by speed
if (foxReady) {
    submitPlayerFox(pid, iron::Vec3{state.x, state.y, state.z}, grounded, dt);
} else {
    submitPlayerCube(pid, iron::Vec3{state.x, state.y, state.z});
}
```

```cpp
// CLIENT branch (around line 1968):
const iron::Vec3 cur = *pos;
// Client doesn't have authoritative `grounded`; derive a coarse approximation
// from y-velocity sign over the last frame. If we have no previous sample
// for this pid yet, treat as grounded.
auto prevIt = playerPrevPos.find(pid);
const bool grounded =
    (prevIt == playerPrevPos.end()) ? true
                                    : std::abs(cur.y - prevIt->second.y) < 0.05f;
if (foxReady) {
    submitPlayerFox(pid, cur, grounded, dt);
} else {
    submitPlayerCube(pid, cur);
}
```

`dt` here is the existing render-loop frame delta. Look near the top of the render lambda for the variable name. If it isn't already in scope, add it (the camera update is using something similar — reuse that).

- [ ] **Step 7: Clean up per-peer animator on disconnect**

Find the existing peer-disconnect handler (search for `peers.onDisconnect` or `onPeerLeave` or the place where `remotes.erase(pid)` is called). Add right next to those existing erases:

```cpp
playerAnimators.erase(pid);
playerPrevPos.erase(pid);
```

If the disconnect path doesn't exist (peers are not removed at runtime in v1), skip this step and add a one-line comment in the per-peer maps explaining that they live for the application lifetime. Look at how `remotes` itself handles disconnect for guidance — match that pattern.

- [ ] **Step 8: Build and verify clean compile**

```powershell
cmake --build build-vk --target net-shooter --config Debug
```

Expected: clean compile, no warnings.

- [ ] **Step 9: Commit**

```bash
git add games/07-net-shooter/main.cpp games/07-net-shooter/CMakeLists.txt
git commit -m "M25 Task 4: skinned fox players in net-shooter with idle/walk/run state machine"
```

---

## Task 5: Docs + PR

**Files:**
- Modify: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Append an M25 section to `docs/engine/asset-pipeline.md`**

Open the file. Find the closing of the M24 section (search for `## M24 — Animation playback`). Append a new `## M25 — Character animation state machine + net-shooter wiring` section directly after it.

Match the style of M23/M24's sections — same heading depth, same proportion of intro/types/data-flow/limitations/verification-command structure. Cover:

- **What got added:** `GltfModel::findClip` helper, `iron::CharacterAnimator` engine type, net-shooter skinned-player wiring.
- **Engine types added:** `CharacterAnimator` (state-name -> clip map, hard-cut switching, idempotent switchTo).
- **Runtime data flow:** Fox.glb -> loadGltfModel -> findClip(name) -> CharacterAnimator.setClipForState -> switchTo derived from movement -> evaluate(palette) -> SkinnedDrawCall (one per player per frame).
- **Movement state mapping (net-shooter specific):** the speed thresholds and the `!grounded` -> walk rule, plus the rationale that fox has no jump anim.
- **Limitations / non-goals:** hard cut only (no crossfade); zero animation network sync; ragdoll handoff via existing M21 guard.
- **Verification:**
  ```powershell
  # In two separate terminals (host first, then client):
  .\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
  .\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
  ```
  Expected behaviour: each peer renders the other as a fox; standing still → Survey, WASD slow → Walk, WASD fast → Run, jump → Walk in air, kill peer → ragdoll handoff intact.

- [ ] **Step 2: Commit docs and push branch**

```bash
git add docs/engine/asset-pipeline.md
git commit -m "M25: document skinned-character state machine in asset-pipeline.md"
git push -u origin feat/m25-skinned-characters
```

- [ ] **Step 3: Open the PR**

```powershell
gh pr create --title "M25: Skinned fox characters in net-shooter" --body "$(cat <<'EOF'
## Summary

- Added `CharacterAnimator` engine type — state-name → clip map with idempotent hard-cut switching, wrapping the M24 `AnimationPlayer`.
- Added `GltfModel::findClip(name)` lookup helper (closing an M24 final-review note).
- Vendored Fox.glb (Khronos CC0) and wired net-shooter to render each player as an animated fox driven by movement state (idle / walk / run, with walk in-air as the fox-has-no-jump compromise).
- M21 ragdoll handoff preserved unchanged; falls back to the M22 cube path if the asset is missing.

## Test plan

- [x] Unit tests pass locally (`CharacterAnimator` state machine, `findClip` lookup, Fox.glb smoke-load)
- [x] Full test suite green
- [x] net-shooter builds clean
- [ ] Visual verification (host + client): each peer sees the other as a fox; clips switch on movement; kill triggers ragdoll handoff

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Adjust the body to match the actual house style for this repo (the recent M24 PR #44 is the template). Return the PR URL.

---

## Verification Checklist (run before declaring milestone done)

- [ ] `ctest --test-dir build-vk --output-on-failure` — full suite green (expected 40+/40+; M24 was 39/39, this adds at least 5 character_animator tests + 2 gltf_loader tests).
- [ ] Build `net-shooter` clean, no warnings.
- [ ] Visual: host + client connect; each peer sees the other render as a fox; idle clip when stationary; walk clip when moving slowly; run clip when sprinting; walk clip in mid-air; ragdoll on death; respawn returns to idle.
- [ ] No new bandwidth — confirm in logs / network panel that net-shooter packets are unchanged in size from M24 baseline.
- [ ] PR CI green.
