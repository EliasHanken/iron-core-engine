# M47 — Bloom (Design)

**Date:** 2026-06-03
**Branch:** `m47-bloom` (off `main`, M46 IBL fully merged)
**Status:** Approved — ready for implementation plan

## Goal

UE/COD-style HDR bloom: bright highlights (the HDRI sun, emissive helmet glow,
specular hotspots) bleed soft light. Bloom is extracted from the HDR scene color
**before** tonemapping, blurred via a mip-chain (downsample → tent upsample), added
back to the HDR scene, and the existing M44 ACES (Narkowicz) pass tonemaps the sum.
This is the physically-correct order (bloom in linear HDR, one tonemap over the total).

Non-goals: lens dirt textures, anamorphic/streak bloom, temporal accumulation,
per-light bloom masks. (Possible future polish; out of scope for M47.)

## Background (current pipeline, from code exploration)

- **HDR scene target:** `VkPostProcess::sceneColor_` — `VK_FORMAT_R16G16B16A16_SFLOAT`,
  swapchain-sized, `COLOR_ATTACHMENT | SAMPLED`. Geometry/skybox/particles render into
  it via `scenePass_` during `VulkanRenderer::endFrame()`.
- **Tonemap/composite:** a fullscreen-triangle pass. Vertex `kFullscreenVert`, fragment
  `kCopyFrag` (`VkPostProcess.cpp`) applies Narkowicz ACES per channel with an exposure
  push constant (`CopyPush{ float exposure; }`). Reads `sceneColor_` (binding 0), writes
  the LDR `viewportColor_` target inside `viewportPass_`. CPU port lives in `Tonemap.h`
  (tested by `test_tonemap`).
- **Composite chain:** `runChain()` records the active post passes (Copy / Outline /
  GlowComposite / XRay) into `viewportPass_`. `runChainOffscreenPasses()` records
  separable-blur pre-passes (the existing **glow** effect: `glowScratch_[2]` ping-pong,
  standalone render passes) **before** `beginViewportPass()`. This is the template bloom
  follows.
- **Editor/output chain:** `sceneColor_` → ACES composite → `viewportColor_` (sampled by
  the editor's `ImGui::Image`, or blitted to the swapchain for games via
  `setBlitViewportToSwapchain(bool)`).
- **Param plumbing pattern:** `setExposure()` → `VulkanRenderer::pendingExposure_` →
  passed per-frame into `runChain()` → push constant. Bloom knobs mirror this exactly.
- **Resize:** offscreen targets are rebuilt in `createTargets()`/`destroyTargets()` and
  persist across resize (M43b lesson — don't let a resize destroy a render pass mid-life).

## Architecture

A new per-frame HDR bloom pre-pass inside `VkPostProcess`, recorded before the viewport
(tonemap) pass — exactly where the glow blur pre-passes already run. All passes are
fullscreen-triangle fragment passes (consistent with the existing composite infra), all
`RGBA16F`.

### Pass graph (per frame)

```
sceneColor_ (HDR, RGBA16F)
  │  (1) bright + downsample  -> bloomChain mip 0  (half scene res)
  │      soft-knee threshold(threshold, knee) + Karis average (firefly tamer)
  ▼
 mip0 -> mip1 -> mip2 -> ... -> mipN     (2) 13-tap box downsample, each half res
 mipN -> ... -> mip2 -> mip1 -> mip0     (3) 3x3 tent upsample, ADDITIVE blend
  │      scatter = upsample tent spread (controls glow width)
  ▼
bloomChain mip 0 now holds the full accumulated bloom
  │  (4) composite: modified ACES pass samples scene + bloom mip0
  ▼
viewportColor_ = ACES( (scene + bloom * intensity) * exposure )
```

### Components

- **Bloom mip chain target (`bloomChain_`):** one `RGBA16F` image with N mip levels,
  N = computed from the scene extent — halve until the smaller dimension ≈ 8 px, capped
  at 7 levels. Per-mip image views + per-mip framebuffers (same per-mip-view pattern as
  the M46c prefiltered cube). One shared single-color-attachment render pass
  (`bloomPass_`) reused by every mip framebuffer (render-pass compatibility = same format/
  samples/layout).
- **Downsample:** reads mip[i] (linear sampler) → writes mip[i+1]. The **first**
  downsample (sceneColor_ → mip0) is a distinct pipeline that also applies the soft-knee
  threshold and the Karis average (per-tap `1/(1+luma)` weighting) to stop the very bright
  HDR sun (radiance ~66560) from causing firefly flicker.
- **Upsample:** samples mip[i+1] with a 3×3 tent filter and **additively blends** the
  result onto the already-downsampled contents of mip[i] (Vulkan blend = ADD,
  src=ONE dst=ONE). `scatter` lerps the tent spread (glow width). Walks N→0; mip0 ends
  holding the full bloom.
- **Pipelines (3 new + 1 modified):**
  - `bloomBrightDownPipeline_` — sceneColor_ → mip0 (threshold + knee + Karis).
  - `bloomDownPipeline_` — mip[i] → mip[i+1].
  - `bloomUpPipeline_` — mip[i+1] → mip[i] (tent, additive blend).
  - `copyPipeline_` (existing) — extended: binding 1 = bloom mip0 sampler;
    `CopyPush` gains `float bloomIntensity;`. Output = `ACES((scene + bloom*intensity) * exposure)`.
- **Embedded shaders (new):** bright+Karis downsample frag, plain downsample frag (13-tap),
  tent upsample frag; plus the modified `kCopyFrag`. Reuse `kFullscreenVert`.

### Composite order

Bloom mixes into the HDR scene **inside** the existing tonemap Copy pass (sampling the
half-res bloom mip0, linearly upscaled), so a single ACES curve covers scene+bloom — the
correct order. Outline / GlowComposite / XRay composite passes are untouched.

## Knobs & plumbing

Mirror `setExposure`. `VulkanRenderer` gains:

```cpp
float pendingBloomThreshold_ = 1.0f;
float pendingBloomKnee_      = 0.5f;
float pendingBloomIntensity_ = 0.05f;
float pendingBloomScatter_   = 0.85f;
```

with public **concrete (non-virtual) setters** `setBloomThreshold/Knee/Intensity/Scatter(float)`
**on `VulkanRenderer` only** — mirroring `setExposure`, which is a VulkanRenderer-only inline
method, NOT on the abstract `Renderer` base. Post-process knobs are not part of the abstract
interface; the sandbox already accesses them via `static_cast<VulkanRenderer&>(renderer)`
(`vkRenderer`). Threshold + knee feed the bright pass; scatter feeds the upsample; intensity
feeds the composite. Bloom is always on (`intensity = 0` makes it invisible; the ~10 tiny
passes are cheap).

Push-constant structs in `VkPostProcess`:
- `BloomBrightPush { float threshold; float knee; }`
- `BloomUpPush     { float scatter; }`
- `CopyPush` extended with `float bloomIntensity;`

**No abstract-interface change** → no `MockRenderer` override and no pure-virtual clean-build
risk (unlike M46a's `loadHdrSkybox`). Still build all targets before claiming done per
[[verify-clean-build-before-ci]], but the bloom setters touch only `VulkanRenderer`.

## Resize

`bloomChain_`, its per-mip views/framebuffers, and `bloomPass_` are created in
`createTargets()` and torn down in `destroyTargets()` against the scene/swapchain extent
(same lifecycle as `glowScratch_`). Guard degenerate extents: mip count ≥ 1.

## Files

**Modify:**
- `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp` — bloom mip target + per-mip
  views/framebuffers, `bloomPass_`, 3 pipelines, embedded shaders, `runBloomOffscreenPasses()`,
  modified `kCopyFrag` + `CopyPush`, bloom param fields, create/destroy/resize wiring.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — bloom param members + setters,
  call the bloom pre-pass before `beginViewportPass()`, pass intensity into the composite.
- `games/11-sandbox/main.cpp` — 4 ImGui sliders (threshold, knee, intensity, scatter) for
  the visual gate, calling the new `vkRenderer.setBloom*` setters (the sandbox already holds
  a `VulkanRenderer&` via `static_cast`).

Note: bloom setters are VulkanRenderer-only (mirror `setExposure`); **no change to the
abstract `Renderer.h` and no `MockRenderer` override needed.**

**Add:**
- `engine/render/Bloom.h` — CPU port of the soft-knee threshold (and Karis weight),
  lockstep with the GLSL (mirrors `Tonemap.h` / `Ibl.h`).
- `tests/test_bloom.cpp` — endpoint tests + shader compile-checks; registered in CMake.

## Error handling

Standard `VK_CHECK`. Targets rebuilt on resize; degenerate-extent guard (mip count ≥ 1).
No new device-lost surfaces (additive blend + fullscreen passes only). Bloom always reads a
valid `sceneColor_` (already produced by the scene pass earlier in the frame).

## Testing & visual gate

- **`test_bloom` (automated):**
  - Soft-knee threshold curve: below threshold → 0; far above → ≈ passthrough; monotonic
    non-decreasing across the knee region; finite/non-negative.
  - Karis weight sanity (a brighter tap gets a smaller weight; weight in (0,1]).
  - SPIR-V compile-checks for the 3 new fragment shaders (`compileGlsl` → non-empty,
    magic `0x07230203`), mirroring `test_ibl`'s shader compile-checks.
- **Visual gate (human):** the HDRI sun and the emissive helmet glow bleed soft light;
  the 4 sandbox sliders adjust threshold / knee / intensity / scatter live; no banding in
  the glow falloff; no flicker on the bright sun (Karis average working); metals' specular
  hotspots bloom subtly. No validation errors (mip views, blend, render-pass compat).

## Self-review notes

- **Spec coverage:** mip-chain blur (down/up), HDR pre-tonemap composite, soft-knee +
  Karis, 4 knobs — all from the approved brainstorm.
- **Consistency:** follows the glow offscreen-pre-pass pattern, the per-mip-view pattern
  from M46c, and the `setExposure` param pattern. No new architectural surface.
- **No abstract-interface change:** bloom setters are VulkanRenderer-only (mirror
  `setExposure`), so unlike M46a's `loadHdrSkybox` there is no pure-virtual / MockRenderer
  clean-build trap. Still build all targets before done.
- **Render-pass-compat:** one `bloomPass_` reused across per-mip framebuffers (same format/
  samples) is legal; build pipelines against `bloomPass_`.
