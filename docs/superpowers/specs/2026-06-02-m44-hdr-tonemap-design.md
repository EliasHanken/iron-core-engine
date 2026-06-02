# M44 — HDR + Tonemap (design)

**Date:** 2026-06-02
**Status:** approved (brainstorm), pending implementation plan
**Track:** Render-polish detour (realistic rendering). First milestone of the track.

---

## The render-polish track (context)

A realistic-rendering detour, built one milestone at a time (each its own PR).
Numbering picks up at M44; the editor/scripting track resumes afterward.

| # | Milestone | What lands | Depends on |
|---|-----------|-----------|-----------|
| **M44** | **HDR + tonemap** | Scene renders into an HDR float target; ACES tonemap maps HDR→LDR for display. Foundation — nothing else looks right without it. | M43 chain |
| M45 | PBR (Cook-Torrance) | Metallic-roughness GGX BRDF. `Material` gains metallic/roughness/AO (texture maps **+** scalar fallbacks), replacing `specularMap`/`specPower`. | M44 |
| M46 | IBL | Irradiance + prefiltered specular + BRDF LUT precomputed from the skybox cubemap; replaces flat ambient. | M45, M16 |
| M47 | Bloom | HDR bright-pass → blur chain → additive composite before tonemap. | M44 |
| M48 | SSAO | Depth+normal prepass → screen-space AO → blur → modulates ambient/IBL. | M44 |
| M49 | Displacement (tessellation) | *Optional.* TCS/TES + height map for real geometry detail on PBR surfaces. | M45 |

**Decisions locked during brainstorm (2026-06-02):**
- Plan the whole track, build one milestone at a time.
- PBR target = **metallic-roughness with texture maps + scalar fallbacks** (glTF-standard; reuses CC0 packs).
- Ordering = **realism-first** (PBR → IBL → bloom → SSAO).
- Tessellation = **last / optional**, scoped as displacement mapping.

This spec covers **M44 only**.

---

## M44 problem statement

The scene currently renders into an 8-bit `B8G8R8A8_SRGB` target
(`VkPostProcess::sceneColor_`, sharing `colorFormat_` with the swapchain).
Any radiance above 1.0 — a bright emissive lantern, a sunlit face, a specular
hotspot — is clipped to flat white. PBR, IBL, and bloom all assume an HDR
linear-radiance buffer that preserves values >1.0 and is tone-mapped to the
display range as a final step. M44 introduces that buffer + tonemap. It is a
prerequisite for the rest of the track and a visible improvement on its own
(smooth highlight rolloff instead of clipping).

## Goals
- Scene + radiance-carrying intermediates render in floating-point HDR.
- A single **ACES filmic** tonemap (with an exposure control) maps HDR → LDR at
  the existing composite-into-`viewportColor_` step.
- The app looks equal-or-better; bright sources show smooth rolloff, not clip.
- No new render passes; fits the existing M36/M43a chain.

## Non-goals (deferred)
- Auto-exposure / eye-adaptation (manual `setExposure` only; default 1.0).
- HDR reflection target (`VkReflectionTarget` stays LDR for M44).
- Bloom (M47), PBR (M45) — M44 only makes their prerequisite buffer exist.
- Alternate tonemap operators (Reinhard/AgX); ACES is the single operator.

---

## Architecture

The M36/M43a chain today:

```
scenePass_   : scene geometry + skybox + particles → sceneColor_ (colorFormat_)
mask/glow    : effect intermediates
viewportPass_: composite (copy/outline/glow/xray) sceneColor_ → viewportColor_ (colorFormat_)
               + debug lines + HUD → viewportColor_
swapchain    : blit viewportColor_ (games) OR ImGui samples it (editor)
```

M44 changes two things and adds **no new pass**:

### 1. Split the color format: HDR scene vs. LDR display
- Introduce a distinct `hdrFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT`.
- `sceneColor_` (and `sceneDepth_` stays as-is) uses `hdrFormat_`. So
  `scenePass_`'s color attachment (`VkPostProcess.cpp:611`) becomes `hdrFormat_`.
- `viewportColor_` keeps `colorFormat_` (the swapchain `_SRGB` format) — it is
  what ImGui samples and what blits to games, so it must stay displayable LDR.
- **Render-pass compatibility (the M43b lesson):** changing `sceneColor_`'s
  format changes `scenePass_`'s attachment, so every pipeline that records into
  `scenePass_` must be created against that format. These are: the scene mesh
  pipelines (built against `scenePass_` via `VkPipeline::setScenePass` — already
  centralized in M43b), the skybox pipeline (`VkSkybox`, init'd against
  `scenePass()`), the reflection-into-scene contribution, and the particle
  render pipeline (`VkParticleSystem`, records into `scenePass_`). All share the
  same pass, so consistency is automatic — but they must be re-verified to build
  against `scenePass_`, not the swapchain pass. R16F is a renderable +
  blendable + sampleable format on the target GPU (RTX 5080) and any modern
  Vulkan device; no feature flag needed.

### 2. Fold tonemap into the composite step
- The composite pipelines (copy / outline / glow-composite / xray) sample
  `sceneColor_` (now HDR) and write `viewportColor_` (LDR). Their fragment
  shaders gain, as their **final** output operation:
  `color = ACESFilmic(color * exposure);`
  where `ACESFilmic` is the standard Narkowicz fit
  `(x*(2.51x+0.03)) / (x*(2.43x+0.59)+0.14)` clamped to [0,1].
- The `copy` (passthrough composite) is the default path when no effect is
  active and is the primary place tonemap matters. Outline/glow/xray compose
  their HDR result and apply the same tonemap at the end so every path lands in
  LDR consistently.
- `exposure` is supplied as a push-constant or small uniform to the composite
  pipelines. Default 1.0.

### 3. UI stays out of tonemapping
- Debug lines + HUD continue to record into `viewportColor_` **after** the
  composite/tonemap, in the existing viewport pass. They are authored in
  display (LDR) space and must not be tone-mapped. No change to their flow.

### 4. Exposure control
- `VulkanRenderer::setExposure(float)` (stored as pending state, default 1.0),
  threaded to the composite step. Optionally surfaced on the `Renderer`
  interface; Vulkan-only is acceptable since the polish track is Vulkan-only.
- The editor's Environment panel may later expose an exposure slider (out of
  scope for M44; the setter is the hook).

## sRGB / gamma correctness
- Lighting shaders output **linear** radiance and write no manual gamma (verified
  in sandbox: `outColor = vec4(finalColor, 1.0)`). The composite tonemap also
  outputs linear; the `_SRGB` `viewportColor_` applies the sRGB OETF exactly
  once in hardware on write. **Net: one gamma encode, no double-encode.**
- Action item: confirm the other lit shaders (net-shooter, showcase,
  spinning-cube) follow the same no-manual-gamma pattern before flipping the
  format, since they share `scenePass_`.

## Components touched
- `engine/render/backends/vulkan/VkPostProcess.{h,cpp}` — add `hdrFormat_`;
  `sceneColor_`/`scenePass_` attachment → `hdrFormat_`; composite fragment
  shaders gain exposure + ACES; exposure plumbed in (push-constant/uniform).
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — `setExposure(float)`
  + pending state; pass exposure to the composite step.
- `engine/render/Renderer.h` — optional `setExposure` on the interface.
- Re-verify `VkSkybox`, `VkParticleSystem`, reflection pipelines build against
  `scenePass_` (no format hardcoding).
- New: `engine/render/Tonemap.h` — `acesFilmic(Vec3, exposure)` CPU port for a
  unit test (mirrors the GLSL fit, same pattern as `PointLightMath`/curl-noise).

## Testing
- **Unit (`tests/test_tonemap.cpp`):** ACES CPU port — `aces(0)==0`, monotonic
  increasing, saturates toward 1.0 as input→∞, a known midpoint value, and
  exposure scales input linearly before the curve. CTest count +1.
- **Visual gate:** sandbox + net-shooter look equal-or-better; bright emissive
  lanterns / sunlit faces show smooth highlight rolloff instead of flat white
  clipping; no color shift on mid-tones; no validation errors (Debug layer).

## Risks
- **sRGB double-gamma** — mitigated above; re-verify all lit shaders.
- **Render-pass format consistency** — every `scenePass_` pipeline must match
  `hdrFormat_`; same pass guarantees it, but verify (M43b lesson).
- **Effect composites** (outline/glow/xray) must each apply tonemap or they'll
  emit untone-mapped HDR into the LDR target (banding/clipping). Covered by
  applying ACES in every composite path.
- **Reflection target stays LDR** — reflections won't carry HDR highlights in
  M44; acceptable, noted as a later refinement.

## Open questions for spec review
- Expose `setExposure` on the cross-backend `Renderer` interface, or
  Vulkan-only accessor? (Lean: Vulkan-only — track is Vulkan-only.)
- Push-constant vs. a field in the existing composite UBO for exposure?
  (Lean: push-constant — it's a single float, no descriptor churn.)
