# M50b — Hardware Tessellation + Displacement (fixed factor) — Design

**Date:** 2026-06-04
**Status:** Approved (design), pending spec review
**Milestone:** M50b (second half of the displacement track; follows M50a POM. Distance-adaptive LOD is deferred to **M50c**.)

## Summary

Give the engine its first **hardware tessellation pipeline** and use it to displace real geometry from a height map, then complete the **3-way comparison demo** (flat normal-map / POM / tessellated) so the silhouette win of true displacement over POM is visible. Also add a **global wireframe debug toggle** to visualize the tessellated (and all) geometry.

This is the **biggest pipeline change since the Vulkan backend itself** — the engine has never had tessellation-control/evaluation shader stages, patch-list topology, or tessellation pipeline state.

Scope decisions (confirmed with the user):
- **Fixed (UI-tunable) tessellation factor**, uniform across edges. Distance-adaptive / screen-space LOD + crack-avoidance is **M50c**.
- **Dedicated tessellation pipeline** (vert→tesc→tese→frag), not a retrofit of the shared lit pipeline.
- **Triangle patches** (`patchControlPoints = 3`) — reuses the existing triangle quad mesh as-is, no new mesh format.
- **Reuse the existing lit fragment shader + lit descriptor layout** — the tessellated quad gets the same PBR lighting as the others; only the geometry differs (fair comparison).
- **Global wireframe** toggle (all scene geometry, lit + tess).

Base: branched from `main` (`f566791`) — has M50a's height map at **binding 13** + `heightScale`, and M49's `LitUbo` (1008 bytes with probe fields).

## Architecture

### A. The tessellation pipeline capability

1. **`VkContext` device features** — additionally enable `tessellationShader` and `fillModeNonSolid` (the latter for wireframe), each guarded against the corresponding `supported.*` flag (RTX 5080 supports both). Currently only `shaderClipDistance` is enabled.

2. **`VkShader::compileGlsl`** — extend the stage→`EShLang` mapping to handle `VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT` (`EShLangTessControl`) and `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` (`EShLangTessEvaluation`). (It already handles vertex/fragment/compute.)

3. **Tessellation pipeline** — a 4-stage graphics pipeline (vert, tesc, tese, frag) with `topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`, a `VkPipelineTessellationStateCreateInfo{ patchControlPoints = 3 }`, and otherwise the same render pass / descriptor layout / depth+blend state as the lit pipeline. Because the shared lit `VkPipeline` factory hardcodes 2 stages + triangle-list, the cleanest isolation is: let `VkShader` optionally carry `tesc`/`tese` modules + a patch-control-point count, and have the pipeline factory assemble the tessellation variant (4 stages + patch topology + tess state) when those modules are present. A normal (vert+frag) shader builds exactly as today.

### B. The tessellation shader set (reuses lit descriptor layout + lit frag)

- **vert (tess variant):** pass each control point's attributes (position, normal, uv, tangent) straight through to `tesc` (object/local space; the displacement + MVP happen in `tese`).
- **tesc:** output a **fixed tessellation factor** (a UI-tunable uniform) for all inner + outer levels — uniform, so the quad's two triangles tessellate identically and share consistent edges (no cracks). Pass control-point attributes through.
- **tese (`layout(triangles, equal_spacing, cw)`):** barycentric-interpolate the patch's position/normal/uv/tangent; **sample the height map** (binding 13) at the interpolated UV with `textureLod(...,0)`; **displace** the position along the interpolated (world) normal by `height × displacementScale`; compute the lit-shader varyings (worldPos, normal, uv, tangent, lightSpacePos) and `gl_Position = mvp × displacedPos`. Emits the **exact varying contract (locations 0–4)** the lit fragment shader consumes.
- **frag:** the **unchanged** lit fragment shader (`standardLitFragSource`). For the tessellated draw, POM is disabled by setting its POM gate (`baseColorFactor.w`) to 0 — displacement is real geometry now, not a parallax trick.
- **Displacement scale** rides in a **spare `LitUbo` slot** (`reflectionParams.w`, currently `=0` in the scene pass) — no `LitUbo` growth, no re-touching the std140 layout. The renderer sets it for tessellated draws; the `tese` reads it.

### C. Renderer integration

- A new shader handle (e.g. via `createTessellatedLitShader()` or a flag) whose `VkShader` carries the tesc/tese modules + `patchControlPoints = 3`. A `DrawCall` referencing it routes through the tessellation pipeline.
- `recordSceneDraw` already binds the full lit descriptor set (incl. binding 13 height map). The tessellated draw reuses that path; the only differences are the pipeline (tess) and that `baseColorFactor.w` (POM) is 0 while `reflectionParams.w` (displacement) is set.
- Descriptor stage flags for the UBO/height map may need `…| VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` so `tese` can read the UBO + sample binding 13. (Confirm against the existing layout's `stageFlags`.)

### D. Global wireframe toggle

- **`Renderer::setWireframe(bool)`** — interface method; Vulkan implements, OpenGL/Mock stub. VulkanRenderer stores `pendingWireframe_`.
- **Line-mode pipeline variants:** the lit pipeline factory and the tess pipeline each gain a `VK_POLYGON_MODE_LINE` variant, cached keyed by `(shader, wireframe)`. The per-draw record paths select fill vs line per `pendingWireframe_`. (polygonMode is not core dynamic state, so this is a second cached pipeline — built lazily on first toggle.)
- **Scope:** scene geometry (lit + tessellated). Skybox / HUD / debug-lines stay solid (separate pipelines).
- **Toggle key:** the sandbox binds a free function key (e.g. **F2**, edge-detected like the existing key handling) → `renderer.setWireframe(...)`.

### E. The 3-way demo

Extend M50a's two quads to **three** side-by-side quads of the same stone surface, viewed at a grazing angle, labeled:
- **"Normal map (flat)"** — `heightScale = 0` (M50a flat).
- **"POM"** — M50a parallax (`heightScale > 0`).
- **"Tessellation"** — the tess pipeline with displacement (`displacementScale > 0`, POM off).

Payoff: toggling wireframe (F2) shows the tessellated quad's subdivided, displaced triangles; at a grazing angle its **silhouette/edge actually bumps** (real geometry), which POM's flat outline fundamentally cannot do.

## Data flow

```
Tessellated DrawCall (shader = tessellatedLitShader, material.heightMap + displacementScale)
  → recordSceneDraw: bind lit descriptor set (UBO + maps incl. height@13);
                     ubo.baseColorFactor.w = 0 (POM off); ubo.reflectionParams.w = displacementScale;
                     select tess pipeline (line variant if wireframe)
  → vert: passthrough control-point attrs
  → tesc: emit fixed tess factor (uniform)
  → tese: barycentric interp → sample height@13 → displace along normal by height*displacementScale
          → emit lit varyings (0–4) + gl_Position
  → frag: unchanged lit shader (POM off since baseColorFactor.w==0)
```

## Error handling

- If `tessellationShader` / `fillModeNonSolid` is unsupported (won't happen on the target GPU, but guard anyway): log + skip enabling; the tess shader/pipeline creation fails gracefully and the demo falls back to a normal lit draw (no tessellation). Wireframe toggle becomes a no-op.
- `displacementScale = 0` → `tese` displaces by 0 → flat tessellated quad (still subdivided, just not displaced) — a valid state.
- Tess shader compile failure → shader handle invalid → the tessellated draw is skipped (like any invalid-shader draw), demo shows only flat + POM quads.

## Testing

- Tessellation is GPU-pipeline work → **visual gate** is primary: the tessellated quad shows real geometric relief; wireframe (F2) reveals the subdivided + displaced triangles; at grazing angles its silhouette bumps where POM's stays flat; no cracks across the quad's two triangles; the other two quads + the rest of the scene are unchanged.
- **Unit tests** for the CPU-side helpers where meaningful (e.g. any patch/displacement-scale plumbing); the shared height-map math is already covered by `test_parallax`. Tess/wireframe shader stages aren't unit-testable headless — note that explicitly.
- Build gate: glslang must compile the tesc/tese stages; the pipeline must validate (correct patch topology + tess state + stage flags) with no validation errors.

## Files (anticipated)

- `engine/render/backends/vulkan/VkContext.cpp` — enable `tessellationShader` + `fillModeNonSolid`.
- `engine/render/backends/vulkan/VkShader.{h,cpp}` — tesc/tese stage compilation; `VkShader` carries optional tess modules + patchControlPoints.
- `engine/render/backends/vulkan/VkPipeline.cpp` (or the lit pipeline factory) — assemble the 4-stage patch-list tess pipeline when tess modules present; line-mode variants for wireframe.
- `engine/render/StandardLitShader.h` (or a new `StandardTessShader.h`) — tess vert/tesc/tese sources reusing the lit frag + LitUbo; `createStandardTessellatedLitShader()`.
- `engine/render/Renderer.h` + `VulkanRenderer.{h,cpp}` — `setWireframe(bool)`; tess shader factory; per-draw pipeline selection (tess + wireframe); set `reflectionParams.w` displacement scale + `baseColorFactor.w=0` for tess draws. OpenGL/Mock stubs for `setWireframe` + the tess factory.
- `games/11-sandbox/main.cpp` — third (tessellated) demo quad + labels; F2 wireframe toggle.

## Out of scope (→ M50c / later)

- **Distance-adaptive / screen-space-edge-length tessellation** + crack-avoidance LOD math (M50c).
- Quad-patch tessellation, displacement on arbitrary scene meshes (only the demo surface + any draw that opts into the tess shader).
- Wireframe for skybox/HUD/debug-lines (kept solid by design).
- Generalizing tessellation onto the main lit path for all draws.
