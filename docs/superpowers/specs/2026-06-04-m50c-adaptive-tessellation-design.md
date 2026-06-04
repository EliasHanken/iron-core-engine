# M50c — Distance-Adaptive Tessellation LOD — Design

**Date:** 2026-06-04
**Status:** Approved (design), pending spec review
**Milestone:** M50c (completes the displacement track — adds adaptive LOD to the M50b fixed-factor tessellation pipeline)

## Summary

Upgrade M50b's uniform tessellation factor to **distance-adaptive, screen-space LOD**: each patch edge is subdivided according to its projected on-screen length, so triangles concentrate where they matter (near the camera) and thin out into the distance — the industry-standard hardware-tessellation approach. Add a **receding tessellated ground plane** to show the density gradient, with a **fixed↔adaptive toggle** so the redistribution is visible.

Scope decisions (confirmed with the user):
- **Screen-space (NDC) edge-length** metric (resolution-independent; not raw camera distance).
- **Reuse spare `LitUbo` slots** for the tess config — NO `LitUbo` growth / no 5-block lockstep churn.
- **Receding-grid ground demo + fixed/adaptive toggle (F3)**.
- Keep M50b's fixed-uniform mode available (the toggle's "fixed" side).

Base: branched from `main` (`ad8523a`) — has the full M50b tessellation pipeline (tesc/tese, patch-list, double-sided, wireframe, `setTessellationFactor`, the tess shader sources, the binding-13 height map).

## Architecture

### A. Adaptive factor in the tessellation-control shader (`standardLitTescSource`)

Replace the single `gl_TessLevel* = max(probeBoxMin.w, 1.0)` with per-edge, screen-space-derived factors:

- A **mode** flag selects fixed vs adaptive.
  - **Fixed (mode 0):** all levels = `factor` (M50b behavior, unchanged).
  - **Adaptive (mode 1):** for each triangle edge, compute the **NDC edge length** and derive its factor.
- **Per-edge NDC length:** the tesc has the 3 patch control points in local space (`cpPos[]`) and `u.mvp`. For an edge between control points `a` and `b`:
  ```
  clipA = mvp * vec4(cpPos[a],1);  clipB = mvp * vec4(cpPos[b],1);
  // behind near plane → max tessellation (don't collapse straddling patches)
  if (clipA.w <= 0 || clipB.w <= 0) edgeFactor = maxFactor;
  else {
      vec2 ndcA = clipA.xy / clipA.w;  vec2 ndcB = clipB.xy / clipB.w;
      float ndcLen = length(ndcA - ndcB);              // 0..~2.8 across the screen
      edgeFactor = clamp(ndcLen / targetEdge, 1.0, maxFactor);
  }
  ```
- **Tess-level assignment (triangle domain):** `gl_TessLevelOuter[i]` controls the edge opposite control-point `i` (i.e. the edge between the *other two* control points). Map each outer level to the NDC length of its corresponding edge so **shared edges between adjacent patches get identical factors** (crack-free: the factor is a pure function of the two shared world-space vertices). `gl_TessLevelInner[0] = max(outer0, outer1, outer2)`.
- The `tese` (displacement) is unchanged from M50b.

### B. Config in spare `LitUbo` slots (no growth)

The tess config is per-frame/global; pack it into existing unused slots so `LitUbo` stays 1008 bytes and the 5 GLSL blocks don't change layout:
- `lightCounts.y` = **tess mode** (0 = fixed, 1 = adaptive). (`lightCounts` is `x=pointLightCount, y/z/w = padding` since M15.)
- `lightCounts.z` = **targetEdge** (target NDC edge length; smaller → denser).
- `probeBoxMin.w` = **factor** — the fixed factor in fixed mode, the **max** factor (clamp ceiling) in adaptive mode. (This is M50b's existing tess-factor slot, repurposed to also cap adaptive.)

The `tesc` reads `u.lightCounts.y` (mode), `u.lightCounts.z` (targetEdge), `u.probeBoxMin.w` (factor/max). No other GLSL block needs changes (only the tesc reads these for tessellation; the frag/vert/tese ignore them).

### C. Renderer

- Keep `setTessellationFactor(float)` → writes `probeBoxMin.w` (fixed factor / adaptive max).
- Add `setTessellationMode(bool adaptive)` → `pendingTessMode_` → written to `lightCounts.y` for tess draws.
- Add `setTessellationTargetEdge(float ndc)` → `pendingTessTargetEdge_` (default ~0.08) → `lightCounts.z`.
- In `recordSceneDraw`, for tessellated shaders (existing `sh.tescModule != null` block), also write `lightCounts.y/.z` (the displacement/factor writes from M50b stay). Interface methods are pure-virtual + OpenGL/Mock stubs.

### D. Demo — receding tessellated ground

- Add a **grid mesh builder** (engine `Mesh::appendGrid(origin, size, cells)` or an inline sandbox helper) producing an NxN grid of quads (≈20×20 = 800 quads → 1600 triangle patches), flat (+Y normal), with UVs tiling the stone surface and along-U tangents.
- Place a large ground plane (e.g. 40×40 units) using the tess shader + the stone albedo/height/normal maps, extending toward the horizon from the camera.
- **F3** toggles `setTessellationMode(adaptive)`; **F2** (M50b) toggles wireframe. With wireframe on, fixed mode shows uniform density everywhere (wasteful far away); adaptive shows dense-near / coarse-far. ImGui sliders for max factor + target edge.
- Camera positioned to look across the ground toward the horizon so the gradient is visible.

## Data flow

```
setTessellationMode(adaptive) / setTessellationFactor(max) / setTessellationTargetEdge(t)
  → pending state
  → recordSceneDraw (tess shader): lightCounts.y=mode, lightCounts.z=target, probeBoxMin.w=factor/max
  → tesc: mode==0 ? uniform(factor) : per-edge clamp(ndcLen/target, 1, max)  [crack-free, behind-cam guarded]
  → tese: unchanged (displace along normal by height)  → lit frag
```

## Error handling

- Behind-near-plane endpoints (`clip.w ≤ 0`) → edge clamped to maxFactor (no collapse/flicker on patches crossing the near plane).
- `targetEdge` guarded `> 0` (avoid div-by-zero; clamp to a small min in the tesc).
- Adaptive factor always `clamp(..., 1.0, maxFactor)` → never 0/degenerate, never exceeds the GPU's max tess level (maxFactor ≤ 64).
- Fixed mode (default off / or on, see demo) reproduces M50b exactly — a safe fallback if adaptive looks wrong.

## Testing

- **CPU-port unit test** (`tests/test_tess_lod.cpp` or extend `test_parallax`): a CPU port of the edge-factor function — project two points through an MVP, compute NDC length, `clamp(ndcLen/target, 1, max)`. Validate: a long near edge → high factor; a short/far edge → low factor (→1); behind-camera → maxFactor; monotonic in distance. Locks the math headlessly.
- **Visual gate (sandbox):** wireframe on the receding ground shows a clear **near-dense → far-coarse gradient** in adaptive mode and **uniform** density in fixed mode; **F3** flips between them and the triangles redistribute; **no cracks/seams** between patches at LOD boundaries; the M50b 3-way quads still behave (fixed factor) unless switched.

## Files (anticipated)

- `engine/render/StandardLitShader.h` — adaptive `standardLitTescSource()` (mode + per-edge NDC factors, crack-free, behind-cam guard).
- `engine/render/TessLod.h` (new, header-only) — CPU port of the edge-factor math (lockstep with the tesc) + unit-testable.
- `tests/test_tess_lod.cpp` (+ CMake registration).
- `engine/render/Renderer.h` + `VulkanRenderer.{h,cpp}` — `setTessellationMode` / `setTessellationTargetEdge` (+ pending state, UBO writes); OpenGL/Mock stubs.
- `engine/scene/Mesh.{h,cpp}` (or inline in sandbox) — `appendGrid` helper.
- `games/11-sandbox/main.cpp` — receding tessellated ground + F3 toggle + sliders + camera framing.

## Out of scope (later)

- Continuous/fractional-spacing LOD smoothing (pop-free transitions) — `equal_spacing` is fine for the showcase.
- Per-patch frustum culling.
- Displacement on arbitrary scene meshes (only the demo surfaces opt into the tess shader).
- The displacement track completes with M50c; the next milestone is **Animation blending + IK** (separate brainstorm).
