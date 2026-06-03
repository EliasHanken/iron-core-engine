# M48 — SSAO (Design)

**Date:** 2026-06-03
**Branch:** `m48-ssao` (off `main`, M47 bloom merged)
**Status:** Approved — ready for implementation plan

## Goal

Screen-space ambient occlusion: darken creases/contacts (sphere-floor contacts,
helmet crevices, gaps between objects) to ground them. Hemisphere-kernel SSAO computed
from the (already-sampleable) scene depth, blurred, and multiplied into the scene color
in the existing post-process composite.

Non-goals (deferred / future): HBAO/GTAO, half-resolution SSAO + upsample (perf
optimization), applying AO to ambient-only via a depth pre-pass (the physically-correct
but heavier path — see "Integration choice" below), temporal accumulation, a normal
G-buffer.

## Background (from code exploration)

- **Scene depth is ready:** `VkPostProcess` scene depth is `VK_FORMAT_D32_SFLOAT` created
  with `DEPTH_STENCIL_ATTACHMENT | SAMPLED` usage; the scene render pass stores it and
  ends in `DEPTH_STENCIL_READ_ONLY_OPTIMAL`, so it is sampleable by any pass recorded
  after the scene pass. The x-ray post pass already samples scene depth — mirror that.
  (`sceneDepthView_`, created in `createTargets`.)
- **No normals / no G-buffer:** pure forward renderer. Normals exist only inside the lit
  fragment shader. SSAO must **reconstruct view-space normals from depth** (cross product
  of screen-space derivatives of the reconstructed view position).
- **Forward lit pass:** the lit shader computes the final shaded HDR color (incl. ambient)
  in one pass. There is no depth pre-pass. (This is why we use the post-multiply
  integration — see below.)
- **Camera matrices:** `VulkanRenderer::pendingView_` / `pendingProjection_` (Mat4) are set
  in `beginFrame`. `iron::inverse(const Mat4&)` is a free function in `engine/math/Mat4.h`
  (used by `Picking.cpp`). Post-process passes currently receive no camera matrices — SSAO
  adds a per-frame UBO carrying them.
- **Composite:** the M47 `kCompositeFrag` (separate from the shared blit `kCopyFrag`)
  currently does `aces((scene + bloom*bloomIntensity) * exposure)`, sampling scene (binding 0)
  + bloom mip0 (binding 1) with `CopyPush { float exposure; float bloomIntensity; float _pad[2]; }`.
- **Pattern to mirror:** the M47 bloom pre-pass (`runBloomOffscreenPasses`): offscreen
  fullscreen passes recorded before `beginViewportPass()`, persistent render pass +
  pipelines, per-resize targets rebuilt in `createTargets`/`destroyTargets`, dedicated
  descriptor pool + persistent sets, a dedicated sampler. Frame insertion point is right
  next to the bloom call in `VulkanRenderer::endFrame` (before `beginViewportPass`).

## Integration choice (decided)

**Post-process multiply.** Scene renders fully (color + depth); SSAO is computed from
depth; blurred; then in the composite the scene color is multiplied by the AO factor
before bloom is added. This needs no restructuring of the forward scene-draw loop.

Tradeoff (accepted): AO darkens direct light in creases too (not only ambient/indirect),
a mild and common SSAO-era artifact. The physically-correct alternative (depth pre-pass +
AO applied to the ambient term in the lit shader) was considered and deferred — it requires
rendering opaque geometry twice and a lit-shader binding, out of scope for M48.

## Architecture

Two new fullscreen fragment passes (SSAO + blur) in `VkPostProcess`, recorded before the
viewport pass, plus a composite multiply. All mirror the bloom pre-pass infrastructure.

### Pass graph (per frame)

```
sceneDepth (D32, SHADER_READ after the scene pass)
  │  (1) SSAO pass -> ssaoTex_ (R8, full scene res)
  │      viewPos = reconstruct(uv, depth, invProjection)
  │      N       = normalize(cross(dFdx(viewPos), dFdy(viewPos)))
  │      rotate the hemisphere kernel by the tiled 4x4 noise vector (TBN)
  │      for each of N kernel samples: project to screen, compare depth, range-check
  │      ao = pow(1 - occlusion/N, power)
  ▼
 (2) blur pass: 4x4 box over ssaoTex_ -> ssaoBlurTex_ (R8)   (hides noise tiling)
  ▼
 (3) composite (kCompositeFrag): scene *= mix(1, ssaoBlur, strength); + bloom; ACES
```

### Components
- **Targets:** `ssaoTex_` and `ssaoBlurTex_` — `VK_FORMAT_R8_UNORM`, full scene/swapchain
  extent, `COLOR_ATTACHMENT | SAMPLED`. Per-resize (rebuilt in `createTargets`/destroyed in
  `destroyTargets`). One persistent `ssaoPass_` (single R8 color attachment, loadOp DONT_CARE
  — both passes fully overwrite their output; finalLayout SHADER_READ_ONLY_OPTIMAL), reused
  by both target framebuffers (`ssaoFb_`, `ssaoBlurFb_`).
- **Noise:** a 4x4 `VK_FORMAT_R16G16B16A16_SFLOAT` (or RG16F) texture of random tangent-plane
  rotation vectors `(x in [-1,1], y in [-1,1], 0)`, generated once on the CPU and uploaded;
  sampled with a REPEAT/NEAREST sampler so it tiles. `noiseScale = (extent.w/4, extent.h/4)`.
- **SSAO UBO** (`SsaoUbo`, updated per frame): `Mat4 projection; Mat4 invProjection;
  vec4 kernel[32]; float radius; float bias; float power; float _pad;` (+ `vec2 noiseScale`
  packed in a vec4). The kernel is static (32 hemisphere points); projection/invProjection
  come from the renderer each frame. Allocate as a persistent UBO updated each frame (or from
  the frame ring — match how the renderer updates other per-frame UBOs).
- **Pipelines:** `ssaoPipeline_` (depth + noise + UBO -> AO) and `ssaoBlurPipeline_`
  (ssaoTex -> blurred), both fullscreen-triangle (reuse `kFullscreenVert`), built against
  `ssaoPass_`, blend disabled. Descriptor set layouts: SSAO = {0: depth sampler, 1: noise
  sampler, 2: SsaoUbo}; blur = {0: ssaoTex sampler}.
- **Samplers:** reuse the existing linear-clamp sampler where possible; depth sampled with a
  NEAREST/clamp sampler (the x-ray pass's depth sampler is a precedent — reuse or mirror it);
  noise with a NEAREST/repeat sampler.
- **`runSsaoPass(cb, ...)`:** records pass (1) then pass (2). Mirror `runBloomOffscreenPasses`
  for the begin-pass/viewport/bind/draw/end structure and the dedicated descriptor pool +
  persistent pre-written sets.

### Reconstruction (SSAO shader)
- View position from depth: `clip = vec4(uv*2-1, depth, 1); v = invProjection * clip; viewPos = v.xyz / v.w;`
- Normal: `normalize(cross(dFdx(viewPos), dFdy(viewPos)))` (faceted; adequate for AO).
- Kernel sample: TBN from the reconstructed normal + the noise rotation vector (Gram-Schmidt);
  `samplePos = viewPos + TBN * kernel[i] * radius`; project with `projection`, sample depth at
  the projected uv, compare with a range check (`smoothstep` on `radius / abs(viewPos.z - sampleDepth)`),
  accumulate occlusion when the sampled surface is closer than the sample by more than `bias`.

### Composite
`kCompositeFrag` gains **binding 2 = uSsao** (sampler2D) and an `aoStrength` push field:
```glsl
float ao  = mix(1.0, texture(uSsao, vUV).r, pc.aoStrength);
vec3  hdr = scene * ao + bloom * pc.bloomIntensity;
outColor  = vec4(aces(hdr * pc.exposure), 1.0);
```
`CopyPush` extended to `{ float exposure; float bloomIntensity; float aoStrength; float _pad; }`
(still 16 bytes). The copy descriptor set grows to 3 bindings (0 scene, 1 bloom, 2 ssaoBlur);
the shared `descPool_` sampler count + maxSets bumped accordingly (copy goes 2->3 samplers).
The blit path (`kCopyFrag`) stays untouched.

### Knobs (VulkanRenderer-only, mirror bloom/exposure)
```cpp
float pendingSsaoRadius_   = 0.5f;
float pendingSsaoBias_     = 0.025f;
float pendingSsaoPower_    = 1.5f;
float pendingSsaoStrength_ = 1.0f;
```
Setters `setSsaoRadius/Bias/Power/Strength`. Radius/bias/power feed the per-frame SSAO UBO;
strength feeds the composite (cheap runtime knob, no SSAO recompute). Strength 0 = off
(the SSAO + blur passes still run; ~2 cheap fullscreen passes). Concrete VulkanRenderer
methods — NOT on the abstract `Renderer`, so no `MockRenderer` change.

### Renderer wiring
- Per frame: update the SSAO UBO with `pendingProjection_` and `iron::inverse(pendingProjection_)`
  + radius/bias/power/noiseScale.
- Call `postProcess_.runSsaoPass(cb, ...)` right before `beginViewportPass()` (next to the
  bloom pre-pass call).
- Pass `pendingSsaoStrength_` into the composite (`runChain`/`recordComposite`) alongside the
  existing exposure + bloomIntensity args.

## Resize
`ssaoTex_`/`ssaoBlurTex_` + their framebuffers rebuilt in `createTargets`/`destroyTargets`
(noiseScale recomputed from the new extent each frame via the UBO). Persistent handles
(`ssaoPass_`, pipelines, layouts, noise texture + sampler, descriptor pool/set layouts, UBO)
created once and destroyed in the top-level destroy. (Same persistent-vs-per-resize split as
bloom — watch the descriptor-set-layout creation ordering: create any set layout before the
descriptor sets that use it, the bug caught in M47.)

## Files

**Add:**
- `engine/render/Ssao.h` — CPU hemisphere-kernel generation (`generateSsaoKernel(n)` returning
  view-space sample points: random in the hemisphere `z>0`, length scaled toward the origin via
  `lerp(0.1, 1.0, t*t)`) + a testable `ssaoRangeCheck`/occlusion helper. Lockstep with the GLSL.
- `tests/test_ssao.cpp` — kernel distribution test (all samples `z>=0`, `|v|<=1`, more samples
  near origin) + range-check endpoints + SSAO/blur shader SPIR-V compile-checks. CMake register.

**Modify:**
- `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp` — SSAO/blur targets + framebuffers,
  `ssaoPass_`, 2 pipelines + layouts, noise texture + sampler, SSAO UBO, `runSsaoPass`,
  shader-source accessors (`kSsaoSrc`/`kSsaoBlurSrc`) for compile-checks, the `kCompositeFrag`
  binding-2 + aoStrength change + `CopyPush` extension + copy descriptor layout/pool bump,
  create/destroy/resize wiring.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — SSAO param members + setters,
  per-frame SSAO UBO update, `runSsaoPass` call before the viewport pass, thread aoStrength
  into the composite.
- `games/11-sandbox/main.cpp` — 4 SSAO sliders (radius/bias/power/strength) in the Environment
  panel (next to the Bloom group).

## Error handling
Standard `VK_CHECK`. Targets rebuilt on resize; persistent handles guarded on teardown.
Degenerate-extent guard. Depth sampled in `DEPTH_STENCIL_READ_ONLY_OPTIMAL` (valid after the
scene pass). SSAO/blur passes run unconditionally; `strength = 0` makes them invisible.

## Testing & visual gate
- **`test_ssao` (automated):** kernel samples in the unit hemisphere with origin-weighted
  distribution; range-check/occlusion helper endpoints; SSAO + blur shader compile to SPIR-V.
- **Visual gate (human):** contact/crease darkening (spheres↔floor, between spheres, helmet
  crevices); radius/bias/power/strength sliders adjust live; blur removes the noise tiling
  (no 4x4 grid pattern); no haloing or over-darkening on flat surfaces; no validation errors
  (depth sampling layout, UBO, descriptor bindings).

## Self-review notes
- **Spec coverage:** hemisphere SSAO + reconstructed normals + noise + blur (Architecture),
  post-multiply composite (decided), 4 knobs, `Ssao.h`+`test_ssao`. All from the brainstorm.
- **Consistency:** mirrors the M47 bloom pre-pass infra, the `kCompositeFrag` (not blit)
  composite-edit pattern, and the VulkanRenderer-only knob pattern. No abstract-interface
  change → no MockRenderer change.
- **Known risk:** descriptor-set-layout creation ordering (M47 lesson) — create set layouts
  before allocating sets. Reconstructed (faceted) normals are lower quality than a normal
  buffer but standard for depth-only SSAO; acceptable for M48.
- **Confirmed:** `iron::inverse(Mat4)` exists (`engine/math/Mat4.h:57`); scene depth is
  sampleable; `pendingProjection_` available.
