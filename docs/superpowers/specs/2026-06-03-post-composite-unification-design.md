# Post-Composite Unification (Design)

**Date:** 2026-06-03
**Branch:** `post-composite-unify` (off `main`, M48 SSAO merged)
**Status:** Approved — ready for implementation plan

## Goal

Make bloom (M47) + SSAO (M48) apply in **all** post-process paths. Today they are
composited only in the Copy pass (`kCompositeFrag`), and `planPostChain` runs the
effect passes (outline/xray/glow) *instead of* Copy — so bloom + SSAO vanish whenever
any editor effect is active (e.g. selecting an object activates the Outline effect).

Fix it by making the **Copy composite always run** as the opaque base (scene + bloom +
SSAO + ACES tonemap → LDR `viewportColor_`), and turning the effect passes into **blended
overlays** that draw on top within the same viewport-pass instance.

Also in scope: fix the **single-buffered SSAO UBO** CPU/GPU frame race flagged in M48 review.

Out of scope (deferred): the pre-existing blit `kCopyFrag` push-constant VUID (the separate
swapchain-present path).

## Background (from code exploration)

All four composite passes live in `engine/render/backends/vulkan/VkPostProcess.cpp` as
embedded GLSL, are created in `createXxxPipeline`, and are recorded in `runChain`:

- **Copy** (`kCompositeFrag`): samples scene (b0) + bloom (b1) + ssao (b2); `aces((scene*ao
  + bloom*intensity) * exposure)`; blend OFF; opaque LDR output.
- **Outline** (`kOutlineFrag`): samples scene (b0) + mask `usampler2D` (b1); 3×3 mask edge
  detect; `aces(mix(scene, color, edge) * exposure)`; blend OFF.
- **XRay** (`kXrayFrag`): samples scene (b0) + mask (b1) + maskDepth (b2) + sceneDepth (b3);
  occluded = sceneDepth < maskDepth; `aces(mix(scene, color, occluded*intensity) * exposure)`;
  blend OFF.
- **GlowComposite** (`kGlowCompositeFrag`): samples scene (b0) + blurred coverage (b1) +
  mask (b2); halo = max(blur − solid, 0)*intensity; `aces((scene + color*halo) * exposure)`;
  blend OFF. (GlowBlurH/V run offscreen in `runChainOffscreenPasses` — unchanged.)

`planPostChain` (`engine/render/PostChainPlan.cpp`): returns `{Copy}` when no effects, else
the effect list **without Copy**. `tests/test_postprocess_chain.cpp` unit-tests this.

**Viewport pass:** in `VulkanRenderer::endFrame`, `beginViewportPass` → `runChain` (records
all passes into ONE `viewportPass_` instance, all writing `viewportColor_`) → debug/HUD →
`endViewportPass`. So a Copy draw followed by blended-overlay draws in the same instance is
already the structural model (sequential draws to the same attachment).

**Frame ring:** `VkFrameRing::kFramesInFlight = 2`, `currentIndex()`. The SSAO UBO is
currently a single persistent buffer (`ssaoUboBuf_`/`ssaoUboMapped_`) with a single
`ssaoSet_` — the CPU `updateSsaoUbo` write can race the previous frame's GPU read.

## Architecture

**Copy always runs first as the opaque base; effects become overlays blended on top.**

### 1. `planPostChain` — Copy first, always
```cpp
std::vector<PostPass> passes = {PostPass::Copy};
bool xray=false, outline=false, glow=false;
for (auto k : activeKinds) { /* set flags */ }
if (xray)    passes.push_back(PostPass::XRay);
if (outline) passes.push_back(PostPass::Outline);
if (glow) { passes.push_back(PostPass::GlowBlurH);
            passes.push_back(PostPass::GlowBlurV);
            passes.push_back(PostPass::GlowComposite); }
return passes;
```
`runChain` records Copy first (opaque), then the overlay passes in order. GlowBlurH/V are
still filtered to the offscreen step (`runChainOffscreenPasses`); `runChain`'s switch already
skips them.

### 2. Effect passes → blended overlays
Each effect shader drops `uScene` sampling, the `aces()` function, and the `exposure` push
field, and outputs only its contribution + alpha. Each effect pipeline enables blend.

- **Outline** (`kOutlineFrag`): `outColor = vec4(pc.color.rgb, edge);`
  Blend: `srcColor=SRC_ALPHA, dstColor=ONE_MINUS_SRC_ALPHA, op=ADD` (alpha similar). Keeps
  binding 0 = mask (`usampler2D`) — drops the old scene binding; layout shrinks to 1 binding.
- **XRay** (`kXrayFrag`): `float occluded = (id != 0u && sceneDepth < maskDepth - 1e-4) ? 1.0 : 0.0;
  outColor = vec4(pc.color.rgb, occluded * pc.intensity);`
  Blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Keeps mask + maskDepth + sceneDepth (drops scene);
  layout shrinks to 3 bindings.
- **GlowComposite** (`kGlowCompositeFrag`): `outColor = vec4(pc.color.rgb * halo, 1.0);`
  Blend: additive `srcColor=ONE, dstColor=ONE, op=ADD`. Keeps blur + mask (drops scene);
  layout shrinks to 2 bindings.

The push structs (`OutlinePush`/`XRayPush`/`GlowCompositePush`) drop the `exposure` field.
The shared `descPool_` COMBINED_IMAGE_SAMPLER count is reduced (each effect set loses the
scene sampler). The record code in `runChain` for each effect drops the `exposure` arg and the
scene-binding write. `recordComposite` (Copy) is unchanged.

Pipelines are still built against `viewportPass_`; only blend state + the shader + the
descriptor layout change.

### 3. Behavior change (accepted)
Effects now composite in **LDR over the tonemapped scene** instead of each re-tonemapping
`scene + effect` in HDR. For these stylized editor effects this is equivalent or fine:
- Outline: flat color over the scene at edges — visually equivalent (was `mix` then tonemap;
  now alpha-blend the color over the tonemapped scene).
- XRay: occlusion tint over the scene — equivalent.
- GlowComposite: the halo now adds in LDR rather than HDR (was `aces(scene + color*halo)`).
  Acceptable for a stylized selection glow; the halo color is an authored LDR-ish color.

The intended win: the base scene under any active effect now shows bloom + SSAO.

### 4. SSAO UBO race fix
Replace the single SSAO UBO + set with **per-frame-in-flight** copies:
- `ssaoUboBuf_[kFramesInFlight]`, `ssaoUboAlloc_[kFramesInFlight]`, `ssaoUboMapped_[kFramesInFlight]`.
- `ssaoSet_[kFramesInFlight]` (each set: binding 0 depth + binding 1 noise identical; binding
  2 = that slot's UBO).
- `updateSsaoUbo(int frame, …)` writes/flushes slot `frame`'s buffer.
- `runSsaoPass(VkCommandBuffer cb, int frame)` binds `ssaoSet_[frame]`.
- The renderer passes `frames_.currentIndex()` (the `VkFrameRing` current slot) to both.
- Allocate the kFramesInFlight sets from `ssaoDescPool_` (bump its `maxSets`/pool sizes
  accordingly); the depth/noise/UBO views are written per set in `createSsaoTargets` (depth +
  noise stable across slots; UBO buffer differs per slot). On resize, re-write all slots' sets
  (the depth/ssao views change; the per-slot UBO buffers are persistent).

### 5. Renderer wiring
- `runChain` keeps passing `exposure`/`bloomIntensity`/`aoStrength` (Copy still needs them);
  the effect record code no longer consumes `exposure`.
- `endFrame` passes `frames_.currentIndex()` to `updateSsaoUbo` and `runSsaoPass`.
- No abstract-`Renderer` change; no `MockRenderer` change.

## Files

**Modify:**
- `engine/render/PostChainPlan.cpp` — Copy always first.
- `tests/test_postprocess_chain.cpp` — update expectations: Copy present in every combination.
- `engine/render/backends/vulkan/VkPostProcess.h` — effect push structs drop `exposure`;
  per-frame SSAO UBO arrays + `ssaoSet_[]`; `updateSsaoUbo`/`runSsaoPass` signatures gain a
  frame index.
- `engine/render/backends/vulkan/VkPostProcess.cpp` — rewrite `kOutlineFrag`/`kXrayFrag`/
  `kGlowCompositeFrag` as overlays; enable blend on their pipelines; shrink their descriptor
  set layouts (drop the scene binding) + the `descPool_` sampler count; drop `exposure` +
  scene writes from their record code; per-frame SSAO UBO/sets + frame-indexed update/bind.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` (+`.h` if needed) — pass
  `frames_.currentIndex()` to `updateSsaoUbo`/`runSsaoPass`.

## Error handling
Standard `VK_CHECK`. Per-frame UBO buffers persistent (destroyed once); per-frame sets
allocated from `ssaoDescPool_` and re-written on resize (pool reset, like today). Blend state
is pipeline state — no render-pass change. The overlays read the destination via fixed-function
blending (allowed within a render-pass instance; no read-after-write hazard).

## Testing & visual gate
- **`test_postprocess_chain` (automated, TDD):** `planPostChain` returns Copy first for every
  effect combination (none / outline / xray / glow / combinations), with effects after.
- **Visual gate (human):** select an object → the outline draws AND bloom + SSAO remain
  visible on the scene (the bug); deselect → unchanged; trigger xray + glow effects and confirm
  they still read correctly as overlays; no SSAO UBO validation hazard (and no regression in the
  no-effect path — scene looks identical to before).

## Self-review notes
- **Spec coverage:** Copy-always (planPostChain + test), 3 effect overlays (shaders + blend +
  layouts + record), LDR-overlay behavior note, SSAO UBO race fix, renderer wiring. All from
  the brainstorm + chosen scope.
- **Consistency:** mirrors the existing viewport-pass multi-draw model; effect pipelines stay
  on `viewportPass_`; no abstract-interface change. The per-frame UBO follows the frame-ring
  pattern used elsewhere.
- **Risk:** blend-state correctness per effect (alpha vs additive) and the descriptor-layout
  shrink (drop scene binding) — both verified at the visual gate. The glow LDR-add is the only
  intentional visual delta.
- **Deferred:** blit `kCopyFrag` VUID (separate present path).
