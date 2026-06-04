# M49 — Reflection Probes (static, box-projected) — Design

**Date:** 2026-06-04
**Status:** Approved (design), pending spec review
**Milestone:** M49 (reorders ahead of displacement, which becomes M50)

## Summary

Add **local reflection probes**: scene-capture cubemaps placed at points in a
scene so reflective surfaces show their real local surroundings (walls, floor,
nearby geometry) instead of only the global skybox. This is the Unity/Unreal
"baked reflection probe" model.

Scope decisions for this first milestone (all confirmed with the user):

- **Static baked** — capture the 6 faces on demand (editor "Bake" button) and
  at scene load. No runtime refresh. Reflections do not track moving objects.
- **Box-projected (parallax-correct)** — each probe has a bounding box;
  reflections are parallax-corrected so flat surfaces reflect the right local
  geometry.
- **Nearest single probe + skybox fallback** — an object uses the nearest probe
  whose box contains it; outside all probes it falls back to the existing global
  skybox IBL. No multi-probe blending.

Explicitly deferred (out of scope): real-time refresh, multi-probe blending,
per-axis / SH irradiance from probes.

## Why this fits now

The M46 IBL track already built the exact pipeline a probe needs:

- `VkIblBaker::bakePrefiltered(envCube, faceSize, mipLevels)` turns **any**
  `CubemapHandle` into a roughness-prefiltered specular mip-chain.
- The split-sum BRDF LUT (`initBrdfLut`) is scene-independent and already baked.
- The lit shader (`StandardLitShader.h`) already samples a prefiltered specular
  cube at **binding 11** (`uPrefiltered`) and the BRDF LUT at binding 12, gated
  by `iblEnabled` (`materialParams2.w`).

So the only genuinely new capability is **rendering the scene's 6 faces into a
cubemap from a probe's position**. Everything downstream (prefilter, BRDF LUT,
the shader's specular IBL term) is reused. The runtime integration reuses
binding 11 — a probe substitutes its own prefiltered cube for the skybox's on a
per-draw basis — so **no new descriptor binding is required**.

## Architecture

### Components / units

1. **`ReflectionProbe` (engine/render or engine/scene) — data**
   - Plain struct describing one probe: world center (from the owning entity's
     transform), half-extents (`Vec3` box bounds), face resolution
     (`int`, default 128), intensity (`float`, default 1.0).
   - The baked result: a `CubemapHandle prefiltered` (kInvalidHandle until baked).
   - CPU-only data; no Vulkan types. Unit-testable.

2. **`VkSceneCapture` (engine/render/backends/vulkan) — the new capability**
   - Renders the scene into an offscreen `RGBA16F` cube color target
     (faceSize × faceSize × 6) plus a transient depth buffer.
   - `CubemapHandle capture(VkContext&, VkCubemapStore&, const CaptureScene&,
     Vec3 position, int faceSize)` — runs 6 face passes (90° FOV, view dirs
     matching `cubeFaceDirection()` in `Ibl.h`), returns the radiance cube
     handle stored in `VkCubemapStore`.
   - The capture pass is **simplified, modeled on the planar-reflection pass**
     (M17): sun + ambient + diffuse/normal/spec textures + skybox. It
     deliberately omits:
     - other probes' reflections (prevents probe-sees-probe recursion),
     - SSAO and bloom (those are post-composite, not scene radiance),
     - planar reflection.
   - Replays the same `sceneDraws_` list the normal scene pass records, so a
     capture always matches what is actually in the scene.

3. **`VkIblBaker::bakePrefiltered` (existing) — reused as-is**
   - Probe pipeline: `VkSceneCapture::capture` → radiance cube →
     `bakePrefiltered(radianceCube, faceSize, mipLevels)` → prefiltered cube →
     stored as `ReflectionProbe::prefiltered`. The radiance cube can be freed
     after prefiltering (see leak fix below).

4. **`VkCubemapStore::destroy(CubemapHandle)` (new) — leak fix**
   - Frees an individual baked cube's image/view/allocation. Needed so re-baking
     a probe (or freeing the intermediate radiance cube) does not orphan GPU
     memory. Closes the known "IBL cubemap leak on skybox swap" debt.

5. **Renderer per-draw probe selection (`VulkanRenderer::recordSceneDraw`)**
   - The renderer holds the active probe list (set by the game/editor before the
     frame). For each draw:
     1. Extract object world position from the model matrix translation.
     2. Find the nearest probe whose box AABB contains that position.
     3. If found: bind the probe's prefiltered cube at binding 11 (in place of
        the skybox-derived prefiltered cube), set `probeActive = 1`, upload
        `probeBoxMin`, `probeBoxMax`, `probeCenter` into `LitUbo`.
     4. If not found: existing behavior — skybox-derived prefiltered cube,
        `probeActive = 0`.

6. **Lit shader box-projection (`StandardLitShader.h`)**
   - New `LitUbo` fields (appended; bump the struct size + `static_assert`):
     `probeBoxMin` (vec4), `probeBoxMax` (vec4), `probeCenter` (vec4 — `.w`
     carries `probeActive`).
   - When `probeActive > 0`, before sampling `uPrefiltered`, apply the standard
     box-projection correction to the reflection vector `R`: intersect the
     reflection ray from the fragment world position with the probe AABB, then
     sample toward `(hitPoint − probeCenter)`. When `probeActive == 0`, sample
     `R` directly (current behavior). No new binding.

7. **Editor authoring (engine/editor + sandbox) — M42 pattern**
   - `std::optional<ReflectionProbeDef>` field on `SceneEntity` (mirrors the M42
     `CollisionShape`/`AudioEmitter` components). Reflection-driven Inspector +
     SceneIO come for free. Fields: box half-extents (`Vec3`), face resolution,
     intensity.
   - Green wireframe **box gizmo** in Edit mode, reusing the collider-wireframe
     debug-line path.
   - A **"Bake Reflection Probes"** editor action: iterate probe entities,
     run capture + prefilter for each, store the handle, free the old one via
     `VkCubemapStore::destroy` on re-bake.

## Data flow

**Bake (editor button or scene load, off the hot path):**
```
for each probe entity:
    radianceCube  = VkSceneCapture::capture(ctx, store, scene, probe.center, probe.faceSize)
    prefiltered   = VkIblBaker::bakePrefiltered(ctx, store, radianceCube, probe.faceSize, mips)
    store.destroy(radianceCube)               // intermediate no longer needed
    if (probe.prefiltered valid) store.destroy(probe.prefiltered)   // re-bake frees old
    probe.prefiltered = prefiltered
```

**Runtime (per frame, per draw):**
```
pos   = translation(call.model)
probe = nearestContaining(activeProbes, pos)     // nullptr if none
if probe:
    bind probe.prefiltered at binding 11
    LitUbo.probeActive = 1; probeBoxMin/Max/Center = probe box
else:
    bind skybox prefiltered at binding 11        // current behavior
    LitUbo.probeActive = 0
```

**Shader (specular IBL term, when iblEnabled):**
```
vec3 R = reflect(-V, N);
if (probeActive > 0) R = boxProject(R, worldPos, probeBoxMin, probeBoxMax, probeCenter);
vec3 prefiltered = textureLod(uPrefiltered, R, roughness * maxMip).rgb;
// ... existing split-sum: prefiltered * (F * brdf.x + brdf.y)
```

## Error handling

- Capture / prefilter failures return `kInvalidHandle`; a probe with an invalid
  handle is skipped in selection (object falls back to skybox IBL). The bake
  action logs a warning and continues to the next probe.
- A probe with zero/degenerate box extents is treated as "no probe" (never
  selected) and flagged in the Inspector.
- Selection is defensive: if `activeProbes` is empty, every draw uses the skybox
  path (identical to today's output).
- `VkCubemapStore::destroy` on `kInvalidHandle` is a safe no-op.

## Testing

**Unit tests (headless, CPU-portable — no GPU):**
- Box-projection ray/AABB intersection math (a CPU port mirroring the GLSL, like
  the existing `Ibl.h` CPU ports).
- Nearest-probe-containing selection (point in/out of boxes, nearest of several
  overlapping boxes, empty list → none).
- Cube-face view-matrix generation matches `cubeFaceDirection()` orientation.

**Visual gate (sandbox, Vulkan):**
- A small "room" (walls + floor) with a reflective sphere and one probe whose
  box wraps the room.
- Expect: the **walls are visible in the sphere** (not just the skybox), the
  reflection **parallax-corrects** as the camera/sphere moves within the box,
  and it **degrades to the skybox** reflection when the sphere leaves the box.
- Confirm the "Bake Reflection Probes" button works and re-baking does not leak
  (no growth in cube count / GPU memory across repeated bakes).

## Demo host

`games/11-sandbox` — it already has the editor shell, the IBL HDRI load path,
and PBR materials, so it is the natural place to author a probe and verify.

## Defaults (confirmed)

- Capture/prefilter format: `RGBA16F` (HDR), matching the IBL cubes.
- Default face size: 128² (authorable per probe; 256² available for hero probes).

## Files (anticipated)

- `engine/render/ReflectionProbe.h` — probe data struct + CPU selection/box-math.
- `engine/render/backends/vulkan/VkSceneCapture.{h,cpp}` — 6-face scene capture.
- `engine/render/backends/vulkan/VkCubemap.{h,cpp}` — add `destroy(handle)`.
- `engine/render/StandardLitShader.h` — `LitUbo` probe fields + box-projection.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — per-draw probe
  selection, active-probe list setter, capture orchestration for bake.
- `engine/scene/SceneEntity` (+ reflection registration) — `ReflectionProbeDef`
  optional component.
- Editor: Inspector add/remove component entry, box gizmo, "Bake" action.
- `games/11-sandbox` — demo room + probe + reflective sphere.
- Tests: `tests/test_reflection_probe.cpp` (box-projection, selection,
  face matrices).

## Out of scope (future milestones)

- Real-time probe refresh (every N frames) with a refresh budget/scheduler.
- Multi-probe blending at box boundaries.
- Diffuse (irradiance / SH) contribution from probes — this milestone is
  specular-only; diffuse ambient stays on the global skybox irradiance.
- M50 displacement (parallax/tessellation) follows this milestone.
