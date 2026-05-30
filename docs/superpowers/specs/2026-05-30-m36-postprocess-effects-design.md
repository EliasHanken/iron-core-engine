# M36 — Post-process effects core + selection effects (outline / glow / x-ray) (Design)

**Date:** 2026-05-30
**Milestone:** M36 (editor + renderer track)
**Prerequisite:** M31–M35 gizmo + selection (`iron::Gizmo`, M35 oriented outline);
the Vulkan forward renderer (`VulkanRenderer`) with scene pass, shadow map,
planar reflection, skybox, particles, skinned meshes; `games/11-sandbox` editor host.

## Goal

Build a **reusable post-process framework** — render the scene into an offscreen
color target, tag selected objects into an offscreen mask texture, then run
full-screen passes that read color + mask + depth to composite screen-space
effects. Ship three effects on top of it that together exercise every capability
of the framework:

1. **Silhouette outline** — a crisp, vertex-hugging outline of the selected
   object (replaces the M35 box look for selection emphasis). Proves *edge-detect*.
2. **Glowing outline** — a soft colored halo. Proves *multi-pass separable blur +
   ping-pong*.
3. **X-ray highlight** — the selected object shows through walls. Proves
   *depth-aware compositing*.

The framework is the valuable, reusable part: future effects (hit-flash,
low-health vignette, toon edge-detect on everything, bloom) become
add-a-shader follow-ups on the same chain.

## Context: what real engines do (why this design)

Unreal implements selection/highlight outlines via **Custom Depth / Custom
Stencil** — a *dedicated offscreen buffer* of tagged objects (the `CustomStencil`
holds an 8-bit id → up to 256 categories), sampled by a post-process material
that edge-detects and composites. That is conceptually an **offscreen mask/ID
texture**, which is the path chosen here. True *hardware* stencil bits (in the
main depth-stencil attachment) are used for geometric masking — portals, decals,
region gates, stencil shadow volumes — and remain a separate, later tool (they'd
add a D24S8 attachment then). We are NOT building a deferred G-buffer renderer;
we keep the forward renderer and add only the post-process + tagged-mask slice.

## Scope

**In scope:** offscreen scene-color target; offscreen mask texture written from
tagged draw calls; a full-screen post-process chain with ping-pong scratch
targets; the three effects above; `effectId` on `DrawCall` + `setEffectStyle` on
`Renderer`; reordering UI/overlays to draw after post; sandbox host wiring so the
selected entity gets an effect.

**Out of scope:** deferred/G-buffer rewrite; hardware-stencil masking
(portals/decals); stacking multiple effect kinds on one object id (bitmask);
OpenGL support (Vulkan-only; OpenGL ignores `effectId`); HDR/tonemapping/
scene-wide bloom; saving effect assignments to the scene file.

## Part 1 — Frame restructure (the core)

Today `VulkanRenderer` renders the scene pass directly into the swapchain image
in one render pass. New per-frame shape:

```
1. Scene pass         -> offscreen COLOR target (+ existing D32 depth)
2. Mask pass          -> offscreen MASK texture (tagged draw calls only)
3. Post-process chain -> full-screen passes read color/mask/depth,
                         ping-pong between two scratch color targets
4. Composite          -> swapchain image
5. UI & overlays      -> swapchain, ON TOP (ImGui, HUD, gizmo/overlay lines)
```

Key consequences:

- **Step 5 reordering is deliberate.** Editor chrome (ImGui, HUD, gizmo lines,
  the M35 oriented box) draws *after* post, so effects never bleed onto UI. The
  world gets effects; the UI does not. Today these draw into the scene pass tail;
  they move to a post-composite UI stage on the swapchain.
- **M35 box and the new silhouette coexist.** The M35 oriented box stays an
  overlay line (step 5); the silhouette is a mask-based post effect (step 3).
  They do not conflict. Retiring the box in favor of the silhouette is a future
  choice, not this milestone.
- **When no effects are active**, the chain still runs scene→offscreen→composite
  (one full-screen blit). This keeps a single code path; the blit is cheap at
  1280×720. (An optional fast path that renders straight to swapchain when no
  effect id is active MAY be added if perf warrants — not required for v1.)

**Targets / formats:**
- Scene color: offscreen color image, swapchain color format, sampled in post.
- Depth: existing `D32_SFLOAT` (unchanged).
- Mask: a small offscreen target carrying, per pixel, the **effect id** (R8 or
  R16) AND the tagged object's **own depth** (so post passes can choose to honor
  or ignore scene occlusion — outline/glow use the visible silhouette, x-ray uses
  the un-occluded footprint). Exact packing decided in the plan (candidate:
  R8 id + sample scene depth for occlusion tests, or an R16 id + separate mask-
  depth). The mask pass renders tagged meshes with a trivial shader that writes
  the id.
- Two **ping-pong scratch** color targets for multi-pass effects (glow blur).

All offscreen targets are recreated on `setViewport` (resize), like the existing
reflection/shadow targets.

## Part 2 — Tagging API & host wiring

**Per-object tag — extend `DrawCall`:**
```cpp
struct DrawCall {
    // ... existing fields ...
    uint8_t effectId = 0;   // 0 = no effect; else an index into the effect table
};
```
During the scene pass the renderer already iterates the buffered draw calls; any
call with `effectId != 0` is *also* recorded into the mask pass (writing
`effectId`). No second submit from the host — the mask pass reuses the scene
draw list filtered to tagged calls.

**Effect configuration — renderer API:**
```cpp
struct EffectStyle {
    enum class Kind { Outline, GlowOutline, XRay };
    Kind  kind      = Kind::Outline;
    Vec3  color     = {1.0f, 0.6f, 0.1f};  // selection orange
    float width     = 2.0f;                 // outline thickness / blur radius (px)
    float intensity = 1.0f;                 // glow strength / x-ray opacity
};
// Configure what effect id N looks like. Called rarely (selection/style change).
virtual void setEffectStyle(uint8_t effectId, const EffectStyle& style);
```

The renderer keeps a small table (e.g. 256 entries) of `EffectStyle` indexed by
id. Each frame, the set of *active* ids (those that appeared on a tagged draw
call) determines which post passes run.

**Backend reality (Vulkan-only):** `setEffectStyle` and the `effectId` field get
default no-op/ignored implementations in the base `Renderer`, so OpenGL still
compiles and shipping games are unaffected. Only `VulkanRenderer` implements the
chain. Consistent with the M31–M35 editor-is-Vulkan-gated convention.

**Sandbox host wiring:** once at startup, `setEffectStyle(1, {Kind::Outline,
orange, ...})`. Each frame, set `effectId = 1` on the selected entity's
`DrawCall` (one line in the submit loop). Switching the selected object's effect
(outline/glow/x-ray) = change `style.kind` via `setEffectStyle`. A small UI
affordance (e.g. an Inspector combo or a key) MAY pick the kind; minimum is
outline always-on for the selection. Exact UI decided in the plan.

## Part 3 — The three effects

All are full-screen fragment shaders sampling the mask (id per pixel; 0 =
background), plus scene color and depth. The chain runs the pass for each active
id's `Kind`, compositing onto the running color target.

**1. Silhouette outline (edge-detect).** For each pixel, sample the mask in a
neighborhood sized by `width`. If the pixel's mask is background but a neighbor is
the id (or vice-versa) → it's a silhouette edge → blend `style.color` over scene
color. Crisp line hugging the true rendered silhouette. Uses the *visible* mask
(occlusion-respecting), so only the seen outline is drawn.

**2. Glowing outline (multi-pass blur).** Blur the mask coverage with a
**separable** blur (horizontal pass then vertical pass, ping-ponging the two
scratch targets), subtract the solid interior coverage so the inside doesn't wash
out, add the soft remainder as `style.color` scaled by `intensity`. `width`
scales blur radius. This is the most involved effect (≥2 passes) and is the one
that validates the chain handles sequential ping-pong passes.

**3. X-ray highlight (depth-aware).** The mask captures the object's full screen
footprint regardless of occlusion (un-occluded). The post pass: where mask == id
AND the scene depth at that pixel is nearer than the tagged object (something is
in front) → tint scene color toward `style.color` at `intensity` opacity. Result:
selected object glows through walls. Requires the mask to carry the object's depth
(or a co-rendered mask-depth) so the pass can compare against scene depth.

**One kind per id in v1.** `Kind` selects outline OR glow OR x-ray for a given
id. "Glowing outline" is its own kind that does edge-detect + halo in its pass.
Stacking (e.g. outline + x-ray simultaneously) would make `Kind` a bitmask —
explicitly deferred (YAGNI).

## Part 4 — Testing & verification

**Automated (unit tests; existing 46 stay green, add a few):**
The GPU passes are not unit-testable (same as gizmo/shadows/reflections), but the
pure logic is:
- **Post-chain ordering** — a pure function "given this set of active effect ids
  (with kinds), produce the ordered list of passes to run" — tested directly.
- **Ping-pong target selection** — which scratch target is source vs. dest at
  pass N (pure index math) — tested.
- **Effect-style table** — set/get/clear of `EffectStyle` by id, default values,
  bounds — tested.
- The existing **46 tests must stay green** — the frame restructure touches the
  renderer, so this is the key regression guard.

**Visual (user-verified, the gate):**
- **Outline:** crisp silhouette hugging the object's true shape (helmet reads as a
  helmet, not a box); `width` changes thickness.
- **Glow:** soft halo, interior not washed out; `intensity` changes brightness.
- **X-ray:** selected object behind a wall shows through, tinted.
- **No UI bleed:** ImGui, gizmo handles, HUD, M35 box stay crisp/unaffected.
- **No regressions:** shadows, reflections, fog, skybox, particles, skinned
  meshes all render correctly through the offscreen→composite path.
- **Perf sanity:** smooth at 1280×720.

## File structure

**Engine — new (Vulkan backend):**
- `engine/render/backends/vulkan/VkPostProcess.{h,cpp}` — owns the offscreen
  color/mask/scratch targets, the post render pass(es), the effect pipelines
  (outline / glow-blur / glow-composite / xray), and the chain runner. Recreated
  on resize.
- Post-process shaders (full-screen triangle vertex + per-effect fragment),
  alongside other Vk shaders (inline or `.glsl`, matching the existing
  convention).

**Engine — modified:**
- `engine/render/Renderer.h` — add `EffectStyle`, `uint8_t effectId` on
  `DrawCall`, `virtual void setEffectStyle(...)` with a base no-op.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — render scene to the
  offscreen color target; record the mask pass for tagged calls; run the post
  chain; composite to swapchain; move UI/HUD/overlay emission to after composite;
  recreate targets on `setViewport`; implement `setEffectStyle` + the active-id
  bookkeeping.
- `engine/render/backends/opengl/OpenGLRenderer.*` — inherits the base no-op;
  `effectId` ignored (compile-only; no behavior).

**Engine — new tests:**
- `tests/test_postprocess_chain.cpp` — pass ordering, ping-pong selection,
  effect-style table. Wired via `iron_add_test`.

**Game/editor:**
- `games/11-sandbox/main.cpp` — `setEffectStyle(1, …)` at startup; set
  `effectId = 1` on the selected entity's draw call; optional UI to pick the kind.

**Docs:**
- `docs/engine/renderer.md` (or the nearest existing renderer doc) — document the
  post-process chain, `effectId`/`setEffectStyle`, and the new frame order.
- `docs/engine/editor.md` — note the selection silhouette/glow/x-ray.

## Known limitations (v1)

- One effect kind per object id (no stacking).
- Vulkan-only; OpenGL ignores effects.
- Effect assignment is runtime/host-driven, not saved to the scene file.
- Forward renderer retained (no G-buffer); hardware-stencil masking not included.
