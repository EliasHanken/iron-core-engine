# Post-Composite Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make bloom + SSAO apply in all post-process paths — including when an editor effect (outline/xray/glow) is active — by always running the Copy composite as the opaque base and turning the effect passes into blended overlays. Also fix the single-buffered SSAO UBO CPU/GPU frame race.

**Architecture:** `planPostChain` always emits `Copy` first; `runChain` records Copy (scene+bloom+ssao+tonemap, opaque) then the effect passes as alpha/additive overlays into the same `viewportPass_` instance. Each effect shader drops scene sampling + `aces`/`exposure` and outputs only its contribution + alpha. The SSAO UBO becomes per-frame-in-flight.

**Tech Stack:** Vulkan fullscreen fragment passes (glslang runtime-compiled GLSL), `VkPostProcess` post chain, `VkFrameRing` (kFramesInFlight=2).

---

## Background & conventions (read first)

- Branch `post-composite-unify` off `main` (M48 SSAO merged). Spec: `docs/superpowers/specs/2026-06-03-post-composite-unification-design.md`.
- The four composite passes are embedded GLSL in `engine/render/backends/vulkan/VkPostProcess.cpp`: `kCompositeFrag` (Copy), `kOutlineFrag`, `kXrayFrag`, `kGlowCompositeFrag`. Pipelines: `createCopyPipeline`/`createOutlinePipeline`/`createXRayPipeline`/`createGlowPipelines`. Recorded in `runChain` (the switch on `PostPass`). The push structs are in `VkPostProcess.h` (`CopyPush`/`OutlinePush`/`XRayPush`/`GlowCompositePush`).
- **Current effect shaders all:** sample the HDR scene at binding 0, apply their effect, then `aces(... * exposure)`, blend OFF, opaque output. They each write `viewportColor_`.
- **Viewport pass:** `VulkanRenderer::endFrame` does `beginViewportPass` → `runChain` (records Copy/effects into ONE `viewportPass_` instance, all writing `viewportColor_`) → debug/HUD → `endViewportPass`. Sequential draws to the same attachment already work; the only change is enabling blend on the effect pipelines so they composite over Copy's output instead of overwriting it.
- **`planPostChain`** (`engine/render/PostChainPlan.cpp`) currently returns `{Copy}` when no effects, else the effect list WITHOUT Copy. `tests/test_postprocess_chain.cpp` unit-tests it.
- **SSAO UBO** (`VkPostProcess`): single `ssaoUboBuf_`/`ssaoUboAlloc_`/`ssaoUboMapped_` + single `ssaoSet_`, written each frame by `updateSsaoUbo` and bound in `runSsaoPass`. `VkFrameRing::kFramesInFlight = 2`, `frames_.currentIndex()` gives the current slot (the renderer owns the frame ring as `frames_`).
- **Incrementalism:** after Task 1 (Copy always) but before an effect is converted, that effect still draws opaque and overwrites Copy's output → same look as today for that effect (bloom/ssao still wiped when it's active). No regression; each effect task fixes one effect. Build is the per-task gate; the visual gate (Task 6) is the real confirmation.
- `/W4` is on.

## File structure

**Modify:**
- `engine/render/PostChainPlan.cpp` — Copy always first.
- `tests/test_postprocess_chain.cpp` — update plan expectations (Copy first).
- `engine/render/backends/vulkan/VkPostProcess.h` — drop `exposure` from `OutlinePush`/`XRayPush`/`GlowCompositePush`; per-frame SSAO UBO arrays + `ssaoSet_[]`; frame-index params on `updateSsaoUbo`/`runSsaoPass`.
- `engine/render/backends/vulkan/VkPostProcess.cpp` — rewrite the 3 effect shaders as overlays; enable blend + shrink descriptor layouts + update record/writes; reduce `descPool_` sampler count; per-frame SSAO UBO/sets + frame-indexed update/bind.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` (+`.h` if needed) — pass `frames_.currentIndex()` to `updateSsaoUbo`/`runSsaoPass`.

---

## Task 1: planPostChain — Copy always first (TDD)

**Files:** Modify `engine/render/PostChainPlan.cpp`, `tests/test_postprocess_chain.cpp`.

- [ ] **Step 1: Update the failing tests** — In `tests/test_postprocess_chain.cpp`, replace the four plan tests that assume Copy-is-omitted with Copy-first expectations:
```cpp
static void test_plan_empty_is_just_copy() {
    auto p = iron::planPostChain({});
    CHECK(p.size() == 1); CHECK(p[0] == iron::PostPass::Copy);
}
static void test_plan_outline() {
    auto p = iron::planPostChain({iron::EffectKind::Outline});
    CHECK(p.size() == 2);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::Outline);
}
static void test_plan_glow_is_multipass() {
    auto p = iron::planPostChain({iron::EffectKind::GlowOutline});
    CHECK(p.size() == 4);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::GlowBlurH);
    CHECK(p[2] == iron::PostPass::GlowBlurV);
    CHECK(p[3] == iron::PostPass::GlowComposite);
}
static void test_plan_xray() {
    auto p = iron::planPostChain({iron::EffectKind::XRay});
    CHECK(p.size() == 2);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::XRay);
}
static void test_plan_layer_order() {
    auto p = iron::planPostChain({iron::EffectKind::Outline, iron::EffectKind::XRay, iron::EffectKind::GlowOutline});
    CHECK(p.size() == 6);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::XRay);
    CHECK(p[2] == iron::PostPass::Outline);
    CHECK(p[3] == iron::PostPass::GlowBlurH);
}
```
(Leave the `test_effect_table_*` and `test_pingpong_*` tests unchanged.)

- [ ] **Step 2: Run, verify it fails** — `cmake --build build-vk --config Debug --target test_postprocess_chain && ctest --test-dir build-vk -C Debug -R "^test_postprocess_chain$" --output-on-failure`
Expected: FAIL (current planPostChain omits Copy when effects active).

- [ ] **Step 3: Update `planPostChain`** — In `engine/render/PostChainPlan.cpp`, replace the body:
```cpp
std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds) {
    // Copy always runs first as the opaque base (scene + bloom + SSAO + tonemap).
    // Effects are layered on top as blended overlays.
    std::vector<PostPass> passes{PostPass::Copy};

    bool xray = false, outline = false, glow = false;
    for (auto k : activeKinds) {
        if (k == EffectKind::XRay)             xray = true;
        else if (k == EffectKind::Outline)     outline = true;
        else if (k == EffectKind::GlowOutline) glow = true;
    }
    if (xray)    passes.push_back(PostPass::XRay);
    if (outline) passes.push_back(PostPass::Outline);
    if (glow) {
        passes.push_back(PostPass::GlowBlurH);
        passes.push_back(PostPass::GlowBlurV);
        passes.push_back(PostPass::GlowComposite);
    }
    return passes;
}
```

- [ ] **Step 4: Run, verify it passes** — `cmake --build build-vk --config Debug --target test_postprocess_chain && ctest --test-dir build-vk -C Debug -R "^test_postprocess_chain$" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add engine/render/PostChainPlan.cpp tests/test_postprocess_chain.cpp
git commit -m "post-composite: planPostChain always emits Copy first"
```

---

## Task 2: Outline → blended overlay

**Files:** Modify `engine/render/backends/vulkan/VkPostProcess.h`, `.cpp`.

- [ ] **Step 1: Drop `exposure` from `OutlinePush`** — In `VkPostProcess.h`:
```cpp
    struct OutlinePush { float color[4]; float texel[2]; float width; float _pad; };
```
(Was `{ color[4]; texel[2]; width; exposure; }`. Keep 16-byte-aligned; `_pad` replaces `exposure`.)

- [ ] **Step 2: Rewrite `kOutlineFrag` as an overlay** — In `VkPostProcess.cpp`, replace `kOutlineFrag` with:
```cpp
const char* /* or `static const char* const` matching the existing style */ kOutlineFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform usampler2D uMask;   // overlay: scene comes from the Copy base
layout(push_constant) uniform Push { vec4 color; vec2 texel; float width; } pc;

void main() {
    uint here = texture(uMask, vUV).r;
    float edge = 0.0;
    for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0) continue;
        vec2 o = vec2(float(dx), float(dy)) * pc.texel * pc.width;
        uint n = texture(uMask, vUV + o).r;
        if (n != here) edge = 1.0;
    }
    outColor = vec4(pc.color.rgb, edge);   // alpha-blend over the composited scene
}
)";
```
(Match the exact declaration style of the existing `kOutlineFrag` — likely a file-static in the anonymous namespace. Only the body/bindings change.)

- [ ] **Step 3: Shrink the descriptor set layout to 1 binding (mask at 0)** — In `createOutlinePipeline`, change the outline descriptor set layout from 2 bindings (scene@0 + mask@1) to **1 binding: binding 0 = COMBINED_IMAGE_SAMPLER (mask `usampler2D`), FRAGMENT**. Update the pipeline layout's push range to `sizeof(OutlinePush)` (unchanged size). Read the current 2-binding setup and remove the scene binding, renumbering mask to 0.

- [ ] **Step 4: Enable alpha blend on the outline pipeline** — In `createOutlinePipeline`, set the color blend attachment:
```cpp
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
```
(Adapt to the existing blend-attachment variable name used in `createOutlinePipeline`.)

- [ ] **Step 5: Update the descriptor write + record code** — Where `outlineDescSet_` is written (in `createOutlinePipeline` and the resize re-write path): write only binding 0 = the mask view (`maskColorView_` + `maskSampler_`), removing the scene-binding write. In `runChain`'s `case PostPass::Outline`: remove `pc.exposure = exposure;` (field is gone); keep `color`/`texel`/`width`. Everything else (bind set, push, `vkCmdDraw(cb,3,1,0,0)`) stays.

- [ ] **Step 6: Reduce the shared pool sampler count** — In `createCopyPipeline` (where `descPool_` is created with the COMBINED_IMAGE_SAMPLER count): the outline set now uses 1 sampler instead of 2 → reduce the pool's CIS `descriptorCount` by 1. (Over-sizing is harmless, but reduce for correctness; read the current count expression and adjust. You may defer the single pool-count edit to whichever effect task touches it last — but do it once total across Tasks 2-4: net −3 CIS.)

- [ ] **Step 7: Build** — `cmake --build build-vk --config Debug --target ironcore`. Expected: clean.

- [ ] **Step 8: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "post-composite: outline as alpha-blended overlay (no scene re-tonemap)"
```

---

## Task 3: XRay → blended overlay

**Files:** Modify `engine/render/backends/vulkan/VkPostProcess.h`, `.cpp`.

- [ ] **Step 1: Drop `exposure` from `XRayPush`** — In `VkPostProcess.h`:
```cpp
    struct XRayPush { float color[4]; float intensity; float _pad[3]; };
```
(Was `{ color[4]; intensity; exposure; _pad[2]; }`.)

- [ ] **Step 2: Rewrite `kXrayFrag` as an overlay** — Replace `kXrayFrag` with:
```cpp
const char* kXrayFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform usampler2D uMask;
layout(set = 0, binding = 1) uniform sampler2D  uMaskDepth;
layout(set = 0, binding = 2) uniform sampler2D  uSceneDepth;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;

void main() {
    uint id = texture(uMask, vUV).r;
    if (id == 0u) { outColor = vec4(0.0); return; }     // not tagged: transparent
    float md = texture(uMaskDepth, vUV).r;
    float sd = texture(uSceneDepth, vUV).r;
    float occluded = (sd < md - 1e-4) ? 1.0 : 0.0;      // nearer geometry in front
    outColor = vec4(pc.color.rgb, occluded * pc.intensity);   // alpha-blend tint
}
)";
```
(Bindings shift down by one: mask 1→0, maskDepth 2→1, sceneDepth 3→2; scene binding removed.)

- [ ] **Step 3: Shrink the descriptor set layout to 3 bindings** — In `createXRayPipeline`, change from 4 bindings (scene@0, mask@1, maskDepth@2, sceneDepth@3) to **3: binding 0 = mask (CIS), binding 1 = maskDepth (CIS), binding 2 = sceneDepth (CIS), all FRAGMENT**. Push range `sizeof(XRayPush)`.

- [ ] **Step 4: Enable alpha blend on the xray pipeline** — same blend state as Task 2 Step 4 (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).

- [ ] **Step 5: Update the descriptor write + record code** — Where `xrayDescSet_` is written (init + resize): write binding 0 = mask, 1 = maskDepth, 2 = sceneDepth (drop the scene write, renumber). In `runChain`'s `case PostPass::XRay`: remove `pc.exposure = exposure;`.

- [ ] **Step 6: Build** — `cmake --build build-vk --config Debug --target ironcore`. Expected: clean.

- [ ] **Step 7: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "post-composite: xray as alpha-blended overlay (no scene re-tonemap)"
```

---

## Task 4: GlowComposite → additive overlay

**Files:** Modify `engine/render/backends/vulkan/VkPostProcess.h`, `.cpp`.

- [ ] **Step 1: Drop `exposure` from `GlowCompositePush`** — In `VkPostProcess.h`:
```cpp
    struct GlowCompositePush { float color[4]; float intensity; float _pad[3]; };
```
(Was `{ color[4]; intensity; exposure; _pad[2]; }`.)

- [ ] **Step 2: Rewrite `kGlowCompositeFrag` as an additive overlay** — Replace with:
```cpp
const char* kGlowCompositeFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D  uBlur;
layout(set = 0, binding = 1) uniform usampler2D uMask;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;

void main() {
    float blur  = texture(uBlur, vUV).r;
    float solid = texture(uMask, vUV).r > 0u ? 1.0 : 0.0;
    float halo  = max(blur - solid, 0.0) * pc.intensity;
    outColor = vec4(pc.color.rgb * halo, 1.0);   // additive blend (ONE/ONE) over the scene
}
)";
```
(Bindings: blur 1→0, mask 2→1; scene binding removed. GlowBlurH/V shaders + pipelines are UNCHANGED — they are offscreen, not effect overlays.)

- [ ] **Step 3: Shrink the GlowComposite descriptor set layout to 2 bindings** — In `createGlowPipelines` (the composite pipeline part), change from 3 bindings (scene@0, blur@1, mask@2) to **2: binding 0 = blur (CIS), binding 1 = mask (CIS, `usampler2D`), FRAGMENT**. Push range `sizeof(GlowCompositePush)`. Leave the GlowBlurH/V layouts/pipelines untouched.

- [ ] **Step 4: Enable additive blend on the GlowComposite pipeline** — In the GlowComposite pipeline's blend attachment:
```cpp
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
```
(Only the GlowComposite pipeline; GlowBlurH/V stay blend-OFF.)

- [ ] **Step 5: Update the descriptor write + record code** — Where `glowCompositeDescSet_` is written (init + resize): write binding 0 = blur (scratch[1] view + linear sampler), binding 1 = mask (drop the scene write, renumber). In `runChain`'s `case PostPass::GlowComposite`: remove `pc.exposure = exposure;`.

- [ ] **Step 6: Reduce the shared pool sampler count (net for Tasks 2-4)** — Ensure the `descPool_` COMBINED_IMAGE_SAMPLER `descriptorCount` is reduced by 3 total versus pre-Task-2 (outline −1, xray −1, glow −1). If you didn't adjust it in Tasks 2/3, do the full −3 here. (Over-sizing is harmless; this is for cleanliness/correctness.)

- [ ] **Step 7: Build** — `cmake --build build-vk --config Debug --target ironcore`. Expected: clean.

- [ ] **Step 8: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "post-composite: glow as additive overlay (no scene re-tonemap)"
```

---

## Task 5: SSAO UBO per-frame (race fix) + renderer frame-index wiring

**Files:** Modify `engine/render/backends/vulkan/VkPostProcess.h`, `.cpp`, `engine/render/backends/vulkan/VulkanRenderer.cpp` (+`.h` if needed).

- [ ] **Step 1: Header — per-frame UBO arrays + sets + signatures** — In `VkPostProcess.h`, replace the single SSAO UBO members:
```cpp
    VkBuffer       ssaoUboBuf_[2]    {};      // [VkFrameRing::kFramesInFlight]
    VmaAllocation  ssaoUboAlloc_[2]  {};
    void*          ssaoUboMapped_[2] {};
    VkDescriptorSet ssaoSet_[2]      {};      // one set per frame slot (binding 2 = that slot's UBO)
```
(Use the literal `2` to match `VkFrameRing::kFramesInFlight`; add a `static_assert` against it if `VkFrameRing.h` is includable, otherwise a comment.) Keep `ssaoBlurSet_` single (it only samples ssaoView_, no per-frame data). Change the method signatures:
```cpp
    void updateSsaoUbo(int frame, const Mat4& projection, const Mat4& invProjection,
                       float radius, float bias, float power);
    void runSsaoPass(VkCommandBuffer cb, int frame);
```

- [ ] **Step 2: `.cpp` — create per-frame UBO buffers** — In `createSsaoNoiseAndUbo`, create the host-visible mapped UBO buffer **for each of the 2 slots** (loop `for (int f = 0; f < 2; ++f)`), storing `ssaoUboBuf_[f]`/`ssaoUboAlloc_[f]`/`ssaoUboMapped_[f]`. (Mirror the existing single-buffer creation.) The kernel cache (`ssaoKernel_`) stays single.

- [ ] **Step 3: `.cpp` — allocate + write 2 SSAO sets** — In `createSsaoTargets` where `ssaoSet_` is allocated/written: allocate **2** sets (`ssaoSet_[0]`, `ssaoSet_[1]`) from `ssaoDescPool_` and write each: binding 0 = depth (`sceneDepthView_` + `maskSampler_`, DEPTH_STENCIL_READ_ONLY layout), binding 1 = noise (`ssaoNoiseView_` + `ssaoNoiseSampler_`), binding 2 = `ssaoUboBuf_[f]`. Bump `ssaoDescPool_` `maxSets` to cover 2 ssao sets + 1 blur set (=3) and the pool sizes (COMBINED_IMAGE_SAMPLER count: 2 depth + 2 noise + 1 blur = 5; UNIFORM_BUFFER count: 2). On resize this runs again (pool reset already happens); re-allocate + re-write both sets.

- [ ] **Step 4: `.cpp` — `updateSsaoUbo(frame, …)` writes slot `frame`** — Change the body to write into `ssaoUboMapped_[frame]` and flush `ssaoUboAlloc_[frame]`:
```cpp
    std::memcpy(ssaoUboMapped_[frame], &ubo, sizeof(SsaoUbo));
    vmaFlushAllocation(ctx_->allocator(), ssaoUboAlloc_[frame], 0, sizeof(SsaoUbo));
```

- [ ] **Step 5: `.cpp` — `runSsaoPass(cb, frame)` binds slot `frame`'s set** — In the SSAO (occlusion) pass, bind `ssaoSet_[frame]` instead of `ssaoSet_`. The blur pass still binds `ssaoBlurSet_` (unchanged).

- [ ] **Step 6: `.cpp` — destroy per-frame UBO buffers** — In the top-level `destroy`, destroy both `ssaoUboBuf_[f]`/`ssaoUboAlloc_[f]` (loop), null the mapped pointers. The 2 ssao sets are freed with `ssaoDescPool_` (no per-set free needed).

- [ ] **Step 7: Renderer — pass the frame index** — In `VulkanRenderer::endFrame`, change the SSAO calls to:
```cpp
    const int ssaoFrame = frames_.currentIndex();
    postProcess_.updateSsaoUbo(ssaoFrame, pendingProjection_, iron::inverse(pendingProjection_),
                               pendingSsaoRadius_, pendingSsaoBias_, pendingSsaoPower_);
    postProcess_.runSsaoPass(cb, ssaoFrame);
```
(Confirm the renderer's frame-ring member name — likely `frames_` — and that `currentIndex()` is the slot used for this frame's command buffer. If the renderer advances the ring elsewhere, capture the index consistent with the `cb` being recorded.)

- [ ] **Step 8: Build both backends** — `cmake --build build-vk --config Debug --target ironcore` and `cmake --build build --config Debug --target ironcore`. Expected: both clean.

- [ ] **Step 9: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VulkanRenderer.h
git commit -m "post-composite: per-frame SSAO UBO + sets (fix CPU/GPU frame race)"
```

---

## Task 6: Visual gate

**Files:** none.

- [ ] **Step 1: Build the sandbox** — `cmake --build build-vk --config Debug --target sandbox`.

- [ ] **Step 2: Run + observe** (human gate) — Run `build-vk\games\11-sandbox\Debug\sandbox.exe`. Expected:
- **No selection:** scene looks identical to before (bloom + SSAO present) — no regression in the Copy-only path.
- **Select an object (outline active):** the yellow outline draws AND **bloom + SSAO remain visible** on the scene (the bug is fixed — previously they vanished).
- If xray / glow effects can be triggered in the sandbox: they still read correctly as overlays over the composited (bloom+ssao+tonemapped) scene; glow halo adds over the scene (LDR).
- No validation errors (descriptor layouts, blend, the per-frame SSAO UBO — no sync hazard).

> If an effect overwrites the scene to black/flat instead of overlaying: its pipeline blend wasn't enabled, or its shader still outputs opaque (alpha 1 everywhere). If the outline/tint is missing: the mask binding renumber is wrong. If glow is invisible: additive blend not set or halo binding wrong.

- [ ] **Step 3: (verification only, no commit)**

---

## Task 7: Full verification + review + PR

- [ ] **Step 1: Build ALL + full suite** — `cmake --build build-vk --config Debug` then `ctest --test-dir build-vk -C Debug --output-on-failure`. Expected: all targets compile; all tests pass (incl. the updated `test_postprocess_chain`).

- [ ] **Step 2: Branch review** — `git diff main...HEAD`. Check: planPostChain emits Copy first in all combinations; each effect shader drops scene+aces+exposure and outputs contribution+alpha; blend states correct (outline/xray = SRC_ALPHA/1−SRC_ALPHA, glow = ONE/ONE); descriptor layouts shrank with bindings renumbered + writes updated (init + resize); push structs dropped `exposure` (sizes still match the GLSL push blocks); `descPool_` count consistent; GlowBlurH/V untouched; per-frame SSAO UBO+sets indexed by frame slot, buffers destroyed once, pool sized for 3 sets; renderer passes `frames_.currentIndex()`; both backends build; no abstract-`Renderer`/`MockRenderer` change.

- [ ] **Step 3: PR** (after visual gate passes) — Title `Post-composite unification: bloom + SSAO in all effect paths + SSAO UBO race fix`, base `main`. Note the LDR-overlay behavior change (glow adds in LDR) and that the blit `kCopyFrag` VUID remains deferred.

---

## Self-review notes (plan author)

- **Spec coverage:** Copy-always (Task 1 + test), 3 effect overlays (Tasks 2-4: shader + blend + layout + record + pool), SSAO UBO race fix (Task 5), renderer wiring (Task 5 Step 7), visual gate (Task 6). All spec sections mapped.
- **Incrementalism:** Task 1 alone causes no regression (effects still opaque-overwrite → same as today). Each of Tasks 2-4 fixes one effect; the visual gate confirms the whole.
- **Type/name consistency:** `OutlinePush`/`XRayPush`/`GlowCompositePush` drop `exposure` (sizes stay 16/32/... matching the new GLSL push blocks — verify each GLSL `Push` block matches its struct's leading fields); shaders renumber bindings to start at 0 after dropping scene; `updateSsaoUbo(int frame, …)` / `runSsaoPass(cb, int frame)`; `ssaoUboBuf_[2]`/`ssaoUboMapped_[2]`/`ssaoSet_[2]`; `frames_.currentIndex()`.
- **Riskiest:** the per-effect descriptor-layout shrink + binding renumber + write update (init AND resize paths) and the blend-state enable — all verified at the visual gate. The pool-count edit is shared across Tasks 2-4 (net −3 CIS); over-sizing is harmless so a missed reduction won't break.
- **Context to confirm during implementation:** the exact effect-shader declaration style (anon-namespace file-statics); the blend-attachment + descriptor-layout variable names in each createXxxPipeline; the effect descriptor-set resize re-write sites; the renderer's frame-ring member (`frames_`) + that `currentIndex()` matches the recording `cb`'s slot; whether `VkFrameRing.h` is includable in `VkPostProcess` for a `static_assert(kFramesInFlight == 2)`.
