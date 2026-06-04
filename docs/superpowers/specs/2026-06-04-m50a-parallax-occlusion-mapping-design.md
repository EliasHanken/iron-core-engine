# M50a — Parallax Occlusion Mapping (POM) — Design

**Date:** 2026-06-04
**Status:** Approved (design), pending spec review
**Milestone:** M50a (first half of the displacement track; M50b adds hardware tessellation + the full 3-way comparison)

## Summary

Add **Parallax Occlusion Mapping**: fragment-shader height-field ray-marching that gives flat surfaces convincing apparent depth (recessed mortar lines, cobble gaps) without adding geometry. This is the modern default for material surface detail and integrates into the existing lit fragment shader with no new pipeline stages.

This milestone delivers:
- A **height/displacement map** slot on the material system (+ authorable scale).
- **Full POM** in the lit shader: steep parallax + occlusion interpolation, adaptive step count by view angle. No self-shadowing (deferred).
- A **procedural height generator** for the demo surface (self-contained; the system accepts any height map, so a CC0 displacement pack can be dropped in later).
- A **side-by-side demo** (flat normal-map vs POM) that also scaffolds the M50b 3-way comparison.

Scope decisions (confirmed with the user):
- **Full POM, no self-shadowing.**
- **Procedural height map** for the demo (vs sourcing a CC0 displacement pack).
- **Pack `heightScale` into the spare `baseColorFactor.w`** UBO slot — do NOT grow `LitUbo` (avoids re-touching the std140 layout + all GLSL blocks).

## Why this fits now

The material system already has albedo / normal / metallic-roughness / AO / emissive maps with the same loading + binding + fallback pattern (M13/M45). A height map is one more texture (binding 13) following that exact pattern. POM is purely a fragment-shader addition computed before the other maps are sampled, reusing the TBN basis already built for normal mapping. No pipeline, descriptor-layout-count aside, or render-pass changes.

Base: branched from `main` (`ec3862b`). On this base `LitUbo` is 960 bytes (M45c), `baseColorFactor.w` is the documented spare slot, and the highest lit descriptor binding is 12 — so binding 13 is free.

## Architecture

### Components / units

1. **`Material` / `MaterialDef` (engine/render) — data**
   - `Material`: add `TextureHandle heightMap = kInvalidHandle;` and `float heightScale = 0.05f;`. POM is active for a draw when `heightMap` is valid AND `heightScale > 0`.
   - `MaterialDef` (authorable/serialized): add `std::string heightPath;` and `float heightScale = 0.05f;`, registered via reflection (free Inspector slider + SceneIO). `heightScale` registered with `{.min = 0.0f, .max = 0.2f, .slider = true}`.

2. **Renderer binding (engine/render/backends/vulkan/VulkanRenderer) — binding 13**
   - Descriptor set layout grows to add binding 13 = combined image sampler (`uHeightMap`, fragment stage). Per-frame descriptor pool sampler capacity bumped accordingly (one more sampler per set).
   - A built-in **1×1 flat height fallback** texture (value such that POM is a no-op — see "height convention" below), analogous to `flatNormalTexture`. Bound at 13 when a material has no height map, so existing materials are byte-for-byte unaffected.
   - `recordSceneDraw` + `recordSkinnedDraw` write binding 13 (height map or fallback) and set `baseColorFactor.w = (heightMap valid ? heightScale : 0.0f)`.

3. **Lit shader (engine/render/StandardLitShader.h) — the core**
   - Add `layout(set = 0, binding = 13) uniform sampler2D uHeightMap;` to the fragment shader.
   - Read `heightScale = u.baseColorFactor.w` (spare slot). When `heightScale > 0`:
     - Build the **tangent-space view direction**: `V_ts = normalize(TBN^T * (cameraPos - worldPos))` using the existing TBN (T from `vTangent`, N from `vNormal`, B = cross). (The shader already builds TBN for normal mapping — reuse it.)
     - Ray-march `parallaxOcclusionUV(uv, V_ts, heightScale)`: linear search through `numLayers` depth layers (numLayers = `mix(maxLayers, minLayers, abs(V_ts.z))`, e.g. 8..32), stepping the UV by `V_ts.xy/V_ts.z * heightScale / numLayers` per layer, until the sampled height crosses the current layer depth; then **interpolate** between the last two layers (occlusion interpolation) for a smooth hit. Result = offset UV.
   - The offset UV replaces `uv`/`vUV` for ALL subsequent map samples (albedo, normal, MR, AO, emissive). When `heightScale == 0`, the original UV is used unchanged (POM fully bypassed → no behavior change for non-POM materials).
   - **Height convention:** height map is grayscale where **white = peak, black = valley**; POM treats depth as `1.0 - height` (marches into the surface). The flat fallback texture is therefore white (height 1.0 everywhere → zero parallax → no-op), keeping the bypass exact.
   - The same POM block is added to the skinned fragment path if it shares `standardLitFragSource` (it does — one shared frag source), so no duplication.

4. **`ProceduralTextures` (engine/render) — demo asset**
   - Add a generator producing a strong, tiling height field with clear depth (e.g. rounded cobblestones or a brick-with-deep-mortar pattern) at a usable resolution (e.g. 512²), grayscale. Optionally a matching albedo so the demo reads consistently. Deterministic, no external assets.

5. **Demo (games/11-sandbox)**
   - Two side-by-side quads of the same surface (same albedo/normal/height), tilted toward the camera at a grazing angle so depth is obvious. **Left quad:** `heightScale = 0` (flat, normal-map only). **Right quad:** POM (`heightScale ≈ 0.05`). HUD text labels ("Normal map" / "POM"). Camera positioned to view both at a grazing angle.
   - This demo is the scaffold M50b extends with a third "Tessellated" quad.

## Data flow

```
Material.heightMap + heightScale
   → recordSceneDraw: bind heightMap@13 (or flat fallback), ubo.baseColorFactor.w = heightScale
   → frag shader: if (heightScale > 0)
                     V_ts = TBN^T * normalize(cameraPos - worldPos)
                     uvOffset = parallaxOcclusionUV(vUV, V_ts, heightScale)
                  else uvOffset = vUV
   → all map samples (albedo/normal/MR/AO/emissive) use uvOffset
```

## Error handling

- No height map bound → flat (white) fallback at binding 13 + `baseColorFactor.w = 0` → POM branch not taken → identical output to today.
- `heightScale = 0` (authored) → POM bypassed even if a height map is bound.
- Offset UV is used with the existing sampler (REPEAT wrap), so parallax that walks off the tile wraps consistently with the rest of the material (acceptable for tiling surfaces; the demo surface tiles).
- Degenerate `V_ts.z` near 0 (grazing): `numLayers` is clamped to `maxLayers` and the per-layer step is bounded; no division blow-up (guard `V_ts.z` against a small epsilon).

## Testing

**Unit test (headless, CPU-portable):**
- `tests/test_parallax.cpp` — a CPU port of the parallax-occlusion ray-march (mirroring the `Ibl.h` CPU-port pattern). Validate: a flat (constant) height field returns ~zero UV offset; a known ramp height field returns the expected offset direction/magnitude for a given view vector; grazing-angle layer count clamps. This locks the core math even though the visual result is shader-side.

**Visual gate (sandbox, Vulkan):**
- The two quads, viewed at a grazing angle: the **left** (normal-map) looks flat; the **right** (POM) shows recessed depth (mortar/cobble gaps), and the depth **parallax-shifts correctly as the camera moves**. At the quad's silhouette edge the POM surface stays flat (expected — the known POM limitation that M50b/tessellation fixes).
- Confirm non-POM materials in the scene are visually unchanged (the binding-13 fallback + `heightScale=0` bypass).

## Files (anticipated)

- `engine/render/Material.h` — `heightMap` + `heightScale`.
- `engine/scene/MaterialDef.*` + `MaterialDef.reflect.cpp` — `heightPath` + `heightScale` + reflection registration.
- `engine/render/StandardLitShader.h` — binding 13 + POM ray-march in the fragment source.
- `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}` — binding-13 layout + flat-height fallback + per-draw write + `baseColorFactor.w` = heightScale.
- `engine/render/ProceduralTextures.h` — procedural height (+ albedo) generator.
- `games/11-sandbox/main.cpp` — flat-vs-POM demo quads + labels.
- `tests/test_parallax.cpp` (+ CMake registration) — CPU-port unit test.
- Wherever the MaterialDef→Material resolve happens (sandbox / scene resolve) — carry `heightPath`→load→`heightMap` and `heightScale`.

## Out of scope (later)

- **Self-shadowing** POM (second ray-march toward the light).
- **Silhouette clipping** (inherent POM limitation; addressed by M50b tessellation).
- **Hardware tessellation + displacement** and the full 3-way comparison — that's **M50b**, which extends this demo.
- Per-axis/triplanar height; CC0 displacement asset sourcing (the system supports it, but the demo uses procedural).
