# M44 — HDR + Tonemap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the scene into a floating-point HDR target and tone-map it to LDR with an exposure-controlled ACES filmic curve, so bright values roll off smoothly instead of clipping to white.

**Architecture:** No new render passes. `VkPostProcess::sceneColor_` (and its `scenePass_` color attachment) switch from the 8-bit `colorFormat_` to a new `hdrFormat_ = R16G16B16A16_SFLOAT`. The existing composite-into-`viewportColor_` step (copy/outline/glow/xray pipelines) gains an exposure multiply + ACES tonemap on its final output; `viewportColor_` stays LDR `_SRGB` for display. Debug lines + HUD still render after tonemap (UI, untone-mapped). Exposure is a per-frame push constant driven by `VulkanRenderer::setExposure`.

**Tech Stack:** Vulkan (VMA, glslang runtime GLSL→SPIR-V), C++23, MSVC, CTest. Build dir `build-vk` (no presets). Spec: `docs/superpowers/specs/2026-06-02-m44-hdr-tonemap-design.md`.

---

## File structure

- **Create** `engine/render/Tonemap.h` — header-only `iron::acesFilmic(Vec3 color, float exposure)` CPU port of the GLSL curve (single responsibility: the tonemap math, unit-testable like `PointLightMath.h`).
- **Create** `tests/test_tonemap.cpp` — unit test for `acesFilmic`.
- **Modify** `tests/CMakeLists.txt` — register `test_tonemap`.
- **Modify** `engine/render/backends/vulkan/VkPostProcess.h` — add `hdrFormat_`; add `CopyPush`; repurpose `_pad` → `exposure` in `OutlinePush`/`GlowCompositePush`/`XRayPush`; change `recordComposite`/`runChain` signatures to take `float exposure`.
- **Modify** `engine/render/backends/vulkan/VkPostProcess.cpp` — scene target uses `hdrFormat_`; composite frag shaders gain ACES + exposure; copy pipeline gets a push-constant range; `runChain`/`recordComposite` push exposure.
- **Modify** `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — `setExposure(float)` + `pendingExposure_`; thread exposure into the `runChain` call.

**Build command (all tasks):** `cmake --build build-vk --config Debug`
**Test command (all tasks):** `ctest --test-dir build-vk -C Debug --output-on-failure`

> **Branch:** work happens on `feat/m44-hdr-tonemap` (already created; the spec is committed there).

---

### Task 1: ACES tonemap CPU port + unit test

**Files:**
- Create: `engine/render/Tonemap.h`
- Create: `tests/test_tonemap.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_tonemap.cpp`:

```cpp
// Unit tests for the ACES filmic tonemap CPU port (mirrors the GLSL fit used
// by the composite shaders). Verifies the curve maps 0->0, is monotonic,
// saturates toward 1.0, hits a known midpoint, and that exposure scales input.
#include "render/Tonemap.h"

#include <cassert>
#include <cstdio>

using iron::Vec3;
using iron::acesFilmic;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    // 1. Black maps to black.
    {
        Vec3 r = acesFilmic(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
        assert(approx(r.x, 0.0f) && approx(r.y, 0.0f) && approx(r.z, 0.0f));
    }

    // 2. Monotonic increasing on a grey ramp.
    {
        float prev = -1.0f;
        for (int i = 0; i <= 20; ++i) {
            float v = static_cast<float>(i) * 0.5f;  // 0..10
            float out = acesFilmic(Vec3{v, v, v}, 1.0f).x;
            assert(out >= prev - 1e-6f);
            prev = out;
        }
    }

    // 3. Saturates toward 1.0 for very bright input (never exceeds 1).
    {
        Vec3 r = acesFilmic(Vec3{1000.0f, 1000.0f, 1000.0f}, 1.0f);
        assert(r.x <= 1.0f && r.x > 0.95f);
    }

    // 4. Known midpoint: Narkowicz ACES at x=1.0 -> ~0.8 (a*1+b)/(c*1+d+e).
    //    (2.51+0.03)/(2.43+0.59+0.14) = 2.54/3.16 = 0.803797...
    {
        float out = acesFilmic(Vec3{1.0f, 1.0f, 1.0f}, 1.0f).x;
        assert(approx(out, 0.803797f, 1e-3f));
    }

    // 5. Exposure scales the input before the curve: exposure 2 on x=0.5
    //    equals exposure 1 on x=1.0.
    {
        float a = acesFilmic(Vec3{0.5f, 0.5f, 0.5f}, 2.0f).x;
        float b = acesFilmic(Vec3{1.0f, 1.0f, 1.0f}, 1.0f).x;
        assert(approx(a, b, 1e-4f));
    }

    std::puts("test_tonemap: all passed");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails (does not compile — header missing)**

Run: `cmake --build build-vk --config Debug --target test_tonemap`
Expected: FAIL — `Tonemap.h` not found (and target not yet registered).

- [ ] **Step 3: Create the header**

Create `engine/render/Tonemap.h`:

```cpp
#pragma once

#include "math/Vec.h"

#include <algorithm>

namespace iron {

// ACES filmic tonemap (Krzysztof Narkowicz fit), CPU port of the GLSL used by
// the post-process composite shaders. `exposure` multiplies the linear input
// before the curve. Result is clamped to [0, 1] per channel.
//
// Keep this in lockstep with the GLSL `aces()` in VkPostProcess.cpp.
inline Vec3 acesFilmic(Vec3 color, float exposure) {
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    auto curve = [](float x) {
        float y = (x * (a * x + b)) / (x * (c * x + d) + e);
        return std::clamp(y, 0.0f, 1.0f);
    };
    return Vec3{curve(color.x * exposure),
               curve(color.y * exposure),
               curve(color.z * exposure)};
}

}  // namespace iron
```

- [ ] **Step 4: Register the test**

In `tests/CMakeLists.txt`, find the block where other render tests are registered (e.g. `test_curl_noise` / `test_point_light`) and add an entry following the exact same pattern used there. The canonical pattern in this repo is an `add_executable` + `target_link_libraries(... ironcore)` + `add_test`. Add:

```cmake
add_executable(test_tonemap test_tonemap.cpp)
target_link_libraries(test_tonemap PRIVATE ironcore)
add_test(NAME test_tonemap COMMAND test_tonemap)
```

> If this repo uses a helper macro/function for tests (e.g. `iron_add_test(test_tonemap)`), use that instead — match the surrounding lines verbatim rather than the raw three-line form above.

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build-vk --config Debug --target test_tonemap` then
`ctest --test-dir build-vk -C Debug -R test_tonemap --output-on-failure`
Expected: PASS — `test_tonemap: all passed`, 1/1 test passing.

- [ ] **Step 6: Commit**

```bash
git add engine/render/Tonemap.h tests/test_tonemap.cpp tests/CMakeLists.txt
git commit -m "M44: ACES filmic tonemap CPU port + unit test"
```

---

### Task 2: HDR scene-color format

Flip `sceneColor_` (and thus `scenePass_`'s color attachment) to `R16G16B16A16_SFLOAT`. This is visually neutral on its own (values ≤1.0 display identically; values >1.0 still clamp at the LDR composite output until Task 3 adds tonemap) — it just makes the float buffer exist. `viewportColor_` and all effect intermediates are untouched.

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h:144` (add `hdrFormat_`)
- Modify: `engine/render/backends/vulkan/VkPostProcess.cpp` (scene image + view + scenePass_ attachment use `hdrFormat_`)

- [ ] **Step 1: Add the `hdrFormat_` member**

In `engine/render/backends/vulkan/VkPostProcess.h`, immediately after the `colorFormat_`/`depthFormat_` lines (currently `:144-145`):

```cpp
    VkFormat   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat   depthFormat_ = VK_FORMAT_UNDEFINED;
    // HDR linear-radiance format for the scene target (M44). Scene geometry +
    // skybox + particles render here; the composite step tone-maps it down to
    // the LDR `colorFormat_` viewportColor_. R16G16B16A16_SFLOAT is renderable,
    // blendable, and sampleable on all target GPUs (no feature flag needed).
    VkFormat   hdrFormat_   = VK_FORMAT_R16G16B16A16_SFLOAT;
```

- [ ] **Step 2: Point the scene color image + view at `hdrFormat_`**

In `engine/render/backends/vulkan/VkPostProcess.cpp`, inside `createTargets` (the "Scene color image" block, around `:542` and `:562`), change the two `colorFormat_` uses for the SCENE COLOR image and its view to `hdrFormat_`:

```cpp
        iInfo.format        = hdrFormat_;   // was colorFormat_ (scene color image)
```
```cpp
        vInfo.format                          = hdrFormat_;   // was colorFormat_ (scene color view)
```

> Do NOT change the viewport color image/view (around `:992`/`:1012`) or the viewport-pass attachment (`:1063`) — `viewportColor_` stays `colorFormat_` (LDR display target).

- [ ] **Step 3: Point the scenePass_ color attachment at `hdrFormat_`**

In `createTargets`, the scene render-pass attachment description (around `:611`, the one feeding `vkCreateRenderPass(... &scenePass_)`):

```cpp
    attachments[0].format         = hdrFormat_;   // was colorFormat_ (scenePass_ color attachment)
```

> Confirm via surrounding context this `attachments[0]` is the one used by the `scenePass_` create (the block ending in `&scenePass_`), not the viewport pass (`&viewportPass_`, ~`:1063`).

- [ ] **Step 4: Build and run the full app to verify the format flip is visually neutral**

Run: `cmake --build build-vk --config Debug`
Expected: clean build (only pre-existing LNK4217 warnings).
Run the sandbox: `.\build-vk\games\11-sandbox\Debug\sandbox.exe`
Expected: **no validation errors**, scene renders the same as before (in-range colors identical; this step adds no tonemap yet). Close the window.

> Why no per-pipeline change is needed: scene mesh pipelines build against `scenePass_` (M43b `setScenePass`), skybox (`VkSkybox`, init against `scenePass()`) and particles (`VkParticleSystem`, records into `scenePass_`) all share that one pass, so changing its attachment format keeps them mutually consistent. If validation reports a format mismatch here, it means a pipeline hardcoded `colorFormat_` — find and fix it to use `scenePass_`'s format.

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all pass (56 total: 55 prior + test_tonemap).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "M44: render scene into an R16F HDR target (no tonemap yet)"
```

---

### Task 3: Exposure plumbing + ACES tonemap in the Copy composite (default path)

The copy/composite pipeline is the default path (no effect active). Add a `CopyPush{ float exposure; }`, a push-constant range to its layout, ACES in `kCopyFrag`, and thread exposure from `VulkanRenderer::setExposure` → `runChain` → `recordComposite`.

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` (`CopyPush` struct; `recordComposite`/`runChain` signatures)
- Modify: `engine/render/backends/vulkan/VkPostProcess.cpp` (`kCopyFrag`, copy pipeline layout push range, `recordComposite`, `runChain`)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (`setExposure`, `pendingExposure_`)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (pass exposure to `runChain`)

- [ ] **Step 1: Add `CopyPush` and update signatures in the header**

In `engine/render/backends/vulkan/VkPostProcess.h`, add near the other push structs (after `:128`, the `XRayPush`):

```cpp
    // Push constants for the copy/composite (tonemap) pipeline.
    struct CopyPush {
        float exposure;   // linear exposure multiply applied before ACES
        float _pad[3];
    };
```

Change the `recordComposite` declaration (`:51`):

```cpp
    void recordComposite(VkCommandBuffer cb, float exposure) const;
```

Change the `runChain` declaration (`:70-73`) to add a trailing `float exposure`:

```cpp
    void runChain(VkCommandBuffer cb,
                  const std::vector<PostPass>& passes,
                  const EffectTable& effects,
                  VkExtent2D swapExtent,
                  float exposure);
```

- [ ] **Step 2: Add ACES + exposure to the copy fragment shader**

In `engine/render/backends/vulkan/VkPostProcess.cpp`, replace `kCopyFrag` (`:22-27`) with:

```cpp
const char* kCopyFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform Push { float exposure; } pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 hdr = texture(uScene, vUV).rgb * pc.exposure;
    outColor = vec4(aces(hdr), 1.0);
}
)";
```

- [ ] **Step 3: Give the copy pipeline a push-constant range**

In `createCopyPipeline` (`:1210-1214`), replace the pipeline-layout creation so it includes the `CopyPush` range:

```cpp
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(CopyPush);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &copySetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &copyPipeLayout_));
```

- [ ] **Step 4: Push exposure in `recordComposite`**

Replace `recordComposite` (`:477-482`):

```cpp
void VkPostProcess::recordComposite(VkCommandBuffer cb, float exposure) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copyPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            copyPipeLayout_, 0, 1, &copyDescSet_, 0, nullptr);
    CopyPush pc{};
    pc.exposure = exposure;
    vkCmdPushConstants(cb, copyPipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CopyPush), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
```

- [ ] **Step 5: Thread exposure through `runChain`**

In `runChain` (`:2143`), update the signature to match the header and pass exposure to the Copy case (`:2149-2151`):

```cpp
void VkPostProcess::runChain(VkCommandBuffer cb,
                             const std::vector<PostPass>& passes,
                             const EffectTable& effects,
                             VkExtent2D swapExtent,
                             float exposure) {
```
```cpp
            case PostPass::Copy:
                recordComposite(cb, exposure);
                break;
```

> Outline/Glow/XRay cases still compile (they ignore `exposure` for now — Task 4 wires them).

- [ ] **Step 6: Add `setExposure` + `pendingExposure_` to the renderer**

In `engine/render/backends/vulkan/VulkanRenderer.h`, near `setBlitViewportToSwapchain` (the M43b setter):

```cpp
    // Linear exposure multiply applied before the ACES tonemap in the composite
    // step (M44). Default 1.0. Auto-exposure is deferred.
    void setExposure(float exposure) { pendingExposure_ = exposure; }
```

And with the other pending state members (near `blitViewportToSwapchain_`):

```cpp
    float      pendingExposure_ = 1.0f;
```

- [ ] **Step 7: Pass exposure at the `runChain` call site**

In `engine/render/backends/vulkan/VulkanRenderer.cpp:1033`, update the call:

```cpp
            postProcess_.runChain(cb, passes, effects_, postProcess_.viewportExtent(),
                                  pendingExposure_);
```

- [ ] **Step 8: Build, test, and visually verify highlight rolloff**

Run: `cmake --build build-vk --config Debug`
Expected: clean build.
Run: `ctest --test-dir build-vk -C Debug --output-on-failure` → all pass.
Run the sandbox: `.\build-vk\games\11-sandbox\Debug\sandbox.exe`
Expected: no validation errors; bright areas (emissive lanterns / sunlit faces) now show smooth ACES rolloff (slightly desaturated highlights) instead of flat white clipping; mid-tones look essentially unchanged. Close the window.

- [ ] **Step 9: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M44: ACES tonemap + exposure in the copy composite path"
```

---

### Task 4: ACES tonemap in the outline / glow / xray composites

The effect composites also sample HDR `uScene` and write the LDR target, so each must tone-map its final result too (or effects would emit untone-mapped HDR → clipping/banding). Repurpose each push struct's existing `_pad` slot for `exposure`.

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` (`OutlinePush`, `GlowCompositePush`, `XRayPush`)
- Modify: `engine/render/backends/vulkan/VkPostProcess.cpp` (`kOutlineFrag`, `kGlowCompositeFrag`, `kXrayFrag`, and the three `runChain` cases set `pc.exposure`)

- [ ] **Step 1: Repurpose `_pad` → `exposure` in the three push structs**

In `engine/render/backends/vulkan/VkPostProcess.h`:

`OutlinePush` (`:101-106`) — rename the trailing `_pad` to `exposure`:
```cpp
    struct OutlinePush {
        float color[4];   // rgb outline color, a unused
        float texel[2];   // 1/width, 1/height (of the mask/screen)
        float width;      // outline thickness in pixels
        float exposure;   // M44: tonemap exposure (was _pad)
    };
```

`GlowCompositePush` (`:117-121`) — repurpose the first `_pad` slot:
```cpp
    struct GlowCompositePush {
        float color[4];      // rgb halo color + padding
        float intensity;     // halo strength (style.intensity)
        float exposure;      // M44: tonemap exposure (was _pad[0])
        float _pad[2];
    };
```

`XRayPush` (`:124-128`) — repurpose the first `_pad` slot:
```cpp
    struct XRayPush {
        float color[4];      // rgb tint color + padding
        float intensity;     // tint strength
        float exposure;      // M44: tonemap exposure (was _pad[0])
        float _pad[2];
    };
```

- [ ] **Step 2: Add ACES to the three effect fragment shaders**

In `engine/render/backends/vulkan/VkPostProcess.cpp`:

`kOutlineFrag` — add the `aces` helper, the `exposure` push field, and tonemap the result. Replace the push block + `main`'s final line:
```cpp
layout(push_constant) uniform Push {
    vec4  color;
    vec2  texel;
    float width;
    float exposure;
} pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```
and change the final composite line from
```cpp
    outColor = vec4(mix(scene, pc.color.rgb, edge), 1.0);
```
to
```cpp
    outColor = vec4(aces(mix(scene, pc.color.rgb, edge) * pc.exposure), 1.0);
```

`kGlowCompositeFrag` — add `float exposure;` to its push block and the `aces` helper, then tonemap. Change the push block:
```cpp
layout(push_constant) uniform Push { vec4 color; float intensity; float exposure; } pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```
and change the final line from
```cpp
    outColor = vec4(scene + pc.color.rgb * halo, 1.0);
```
to
```cpp
    outColor = vec4(aces((scene + pc.color.rgb * halo) * pc.exposure), 1.0);
```

`kXrayFrag` — add `float exposure;` to its push block and the `aces` helper, then tonemap both output paths. Change the push block:
```cpp
layout(push_constant) uniform Push { vec4 color; float intensity; float exposure; } pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```
and change BOTH output lines:
```cpp
    if (id == 0u) { outColor = vec4(aces(scene * pc.exposure), 1.0); return; }
```
```cpp
    outColor = vec4(aces(mix(scene, pc.color.rgb, occluded * pc.intensity) * pc.exposure), 1.0);
```

- [ ] **Step 3: Set `pc.exposure` in the three `runChain` cases**

In `runChain` (`:2143`), set `pc.exposure = exposure;` in each effect case before its `vkCmdPushConstants`:
- Outline case: after `pc._pad = 0.0f;` is removed/replaced, add `pc.exposure = exposure;` (it replaced `_pad`).
- GlowComposite case: after the color/intensity assignment, add `pc.exposure = exposure;`.
- XRay case: after the color/intensity assignment, add `pc.exposure = exposure;`.

For the Outline case specifically, the line `pc._pad = 0.0f;` (`:2177`) becomes:
```cpp
                pc.exposure = exposure;
```

- [ ] **Step 4: Build and test**

Run: `cmake --build build-vk --config Debug` → clean build.
Run: `ctest --test-dir build-vk -C Debug --output-on-failure` → all pass.

> Visual verification of effects is exercised in the editor when an entity has an outline/glow/xray effect selected; a quick check is optional since the math mirrors the copy path. If you select an entity with an effect, the effect color should still composite correctly (now tone-mapped).

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "M44: tonemap outline/glow/xray composites for consistency"
```

---

### Task 5: Verify no double-gamma in other games + final gate

The lit shaders in net-shooter / showcase / spinning-cube share `scenePass_`. Confirm none apply a manual gamma (which would double-encode now that scene color is linear R16F and the display target encodes sRGB). The sandbox was already verified (`outColor = vec4(finalColor, 1.0)`).

**Files:**
- Inspect only (no change expected): `games/07-net-shooter/main.cpp`, `games/03-showcase/main.cpp`, `games/01-spinning-cube/main.cpp`

- [ ] **Step 1: Grep each game's fragment shader for manual gamma**

Run a search for gamma/pow patterns in the Vulkan shader strings of the three games:
- Look for `pow(`, `2.2`, `0.4545`, `1.0/2.2`, or any `gamma` in the fragment shader source.
- Expected: the final output is `outColor = vec4(<linear color>, 1.0)` with NO manual gamma (matches sandbox). The `_SRGB` swapchain/viewport target does the single encode.

- [ ] **Step 2: If a manual gamma exists, remove it**

If any game applies `pow(color, vec3(1.0/2.2))` (or similar) before writing `outColor`, delete that operation — the `_SRGB` target already encodes. (Do NOT touch the OpenGL `#else` branch of any shader; OpenGL is frozen and its target handling differs.) If none exists, no change.

- [ ] **Step 3: Build everything and run the full test suite**

Run: `cmake --build build-vk --config Debug` → clean build (only pre-existing LNK4217 warnings).
Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: **56/56 pass**.

- [ ] **Step 4: Final visual gate**

Run the sandbox: `.\build-vk\games\11-sandbox\Debug\sandbox.exe`
Optionally run net-shooter: `.\build-vk\games\07-net-shooter\Debug\net-shooter.exe`
Expected: scenes render correctly, no validation errors, bright highlights roll off smoothly. Hand to the user for the visual gate sign-off (no color shift, no over-darkening, no clipping).

- [ ] **Step 5: Commit (only if a game shader changed)**

```bash
git add games/<changed>/main.cpp
git commit -m "M44: drop redundant manual gamma (sRGB target encodes)"
```

> If no game shader changed, skip this commit — Task 5 was verification only.

---

## Self-review notes (for the implementer)

- **Spec coverage:** HDR format (Task 2), ACES tonemap + exposure (Tasks 1, 3, 4), UI-after-tonemap (unchanged flow, Task 3 note), sRGB single-encode (Task 5), `setExposure` (Task 3), unit test (Task 1), visual gate (Tasks 3, 5). The "open questions" from the spec are resolved: exposure is **Vulkan-only** (`setExposure` on `VulkanRenderer`, not the `Renderer` interface) and a **push-constant** (not a UBO field).
- **Keep GLSL `aces()` and `Tonemap.h::acesFilmic` identical** — same constants (2.51/0.03/2.43/0.59/0.14). If you tune one, tune both.
- **Render-pass compatibility (M43b lesson):** only `scenePass_`'s attachment format changes; all pipelines recording into it share that pass, so they stay compatible. If validation complains, a pipeline hardcoded `colorFormat_` instead of using `scenePass_` — fix that pipeline.
- **No new passes, no descriptor-set changes** — exposure rides as a push constant; the copy pipeline is the only one gaining a push range.
