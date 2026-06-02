# M45b — Cook-Torrance PBR (design)

**Date:** 2026-06-02
**Status:** approved (brainstorm), pending implementation plan
**Track:** Render-polish detour → M45 PBR, second half. **M45a (consolidate lit shader) is merged**; M45b swaps that one shared shader from Blinn-Phong to physically-based shading.

---

## Position in the track

Render-polish roadmap (realism-first): M44 HDR+tonemap ✓ → **M45 PBR** (M45a consolidate ✓ → **M45b this spec**) → M46 IBL → M47 bloom → M48 SSAO → M49 displacement (optional). M45b is the biggest single realism unlock: direct lighting goes physically-based (metallic-roughness GGX).

Because M45a consolidated the lit shader into one engine-owned source, M45b changes shading in **exactly one fragment shader** instead of six.

## Problem

The engine lights surfaces with Blinn-Phong (`specularMap` + `specPower`). That doesn't match the project's chosen realistic direction ([[visual-style-direction]]), can't consume the metallic-roughness PBR materials that glTF/Polyhaven assets ship, and looks wrong under the new HDR pipeline. M45b replaces it with the metallic-roughness Cook-Torrance model (the industry/glTF standard).

## Goals
- Cook-Torrance GGX direct lighting (sun + point lights) replacing Blinn-Phong, in the one shared `standardLitFragSource`.
- Metallic-roughness material model (glTF-canonical: combined MR map + separate AO map + scalar factors).
- Correct linear/sRGB texture handling (a linear-load path for non-color maps).
- CC0 packs render as PBR; a sandbox sphere grid for tuning.
- Pixel-plausible, energy-conserving output (HDR from M44, tone-mapped for display).

## Non-goals (deferred)
- **glTF MR-texture extraction → M45c.** `GltfLoader` already resolves the MR path (`GltfMaterialPaths.metalRoughness`); wiring it into the engine material + gltf-viewer is a small follow-up. In M45b, glTF models render with scalar metallic/roughness defaults.
- **Image-based lighting (IBL) → M46.** Ambient stays the flat ambient term (× AO); the existing cubemap/planar reflection stays a crude reflection mix (proper Fresnel-weighted specular IBL is M46).
- Anisotropy, clearcoat, sheen, transmission (advanced PBR extensions) — not now.

---

## Decisions locked in brainstorm (2026-06-02)
- Texture model = **glTF-canonical**: one combined `metallicRoughnessMap` (`.g`=roughness, `.b`=metallic) + separate `aoMap` (`.r`), each × scalar factor.
- Migration = **clean replace**: drop `specularMap`/`specPower`; old saved scenes load with PBR defaults; no legacy model kept.
- glTF MR extraction = **deferred to M45c**.

---

## Architecture

### 1. Render material — `engine/render/Material.h`
Drop `specularMap`, `specPower`. Add:
```cpp
    float metallic  = 0.0f;    // 0 = dielectric, 1 = metal
    float roughness = 0.5f;    // 0 = mirror-smooth, 1 = fully rough
    float ao        = 1.0f;    // baked ambient-occlusion factor
    TextureHandle metallicRoughnessMap = kInvalidHandle;  // .g = roughness, .b = metallic (glTF)
    TextureHandle aoMap                = kInvalidHandle;  // .r = occlusion
```
Keep `texture` (albedo), `normalMap`, `emissive`, `reflectivity`, `useReflectionPlane`, `uvScale`.

`OpenGLRenderer.cpp` references `call.material.specPower` (frozen backend). Remove those references so the GL backend still **compiles** (GL demo games don't author specular maps; look unaffected). No new PBR work on the OpenGL path.

### 2. Linear vs sRGB texture loading (correctness prerequisite)
`VkTexture` currently hardcodes `VK_FORMAT_R8G8B8A8_SRGB` for ALL textures — wrong for non-color data (normal/MR/AO). Add a color-space choice:
- `Renderer::createTexture(w, h, data, bool srgb = true)` and `loadTexture(path, bool srgb = true)` (default `true` preserves albedo behavior). Vulkan backend picks `R8G8B8A8_SRGB` (srgb) vs `R8G8B8A8_UNORM` (linear). OpenGL backend takes the param too (frozen; can ignore/approximate).
- Load albedo as sRGB; load **normal, MR, AO as linear**. This incidentally fixes the existing normal-map load (currently sRGB → slightly wrong normals).

### 3. Editor material — `engine/scene/SceneFormat.h` `MaterialDef` + `MaterialDef.reflect.cpp`
Drop `specularPath`; add `std::string metallicRoughnessPath`, `std::string aoPath`, `float metallic = 0.0f`, `float roughness = 0.5f`, `float ao = 1.0f`. Update `MaterialDef.reflect.cpp` field registrations. Because Inspector + SceneIO are reflection-driven (M38/M39), the Inspector UI and scene (de)serialization update **automatically**. Old scenes: missing PBR fields → defaults; `specularPath` ignored. Sandbox's `resolveTexture` material wiring (`main.cpp:~260`) updates to resolve the new maps (MR/AO loaded **linear**).

### 4. `LitUbo` + descriptor set layout (`VulkanRenderer`)
- `materialParams.y` (was `specPower`) → **roughness**. Append `Vec4 materialParams2 { metallic, ao, 0, 0 }`. `LitUbo` 928 → 944 bytes; `static_assert` updated.
- Descriptor binding 3 (`uSpecularMap`) → `uMetallicRoughnessMap`; **add binding 7** `uAoMap`. Layout 7 → 8 bindings.
- `VkFrameRing` sampler-pool capacity 6× → 7× `kMaxDescriptorSetsPerFrame`.
- `recordSceneDraw` writes the new scalars into the UBO and binds MR/AO maps, defaulting **missing maps to `whiteTexture()`** (glTF convention: absent texture = 1.0, so `roughness = factor×1`, `metallic = factor×1`, `ao = 1`).

### 5. Shading — `engine/render/StandardLitShader.h` (`standardLitFragSource`)
Replace the Blinn-Phong block with Cook-Torrance GGX:
- Sample + combine: `roughness = u.materialParams.y * texture(uMetallicRoughnessMap, uv).g`; `metallic = u.materialParams2.x * texture(uMetallicRoughnessMap, uv).b`; `ao = u.materialParams2.y * texture(uAoMap, uv).r`. Albedo from `uDiffuse`; normal via existing TBN + `uNormalMap`.
- `F0 = mix(vec3(0.04), albedo, metallic)`.
- Per light (sun, then each point light): `D` = Trowbridge-Reitz GGX, `G` = Smith with Schlick-GGX, `F` = Fresnel-Schlick; `spec = D*G*F / (4*NdotV*NdotL + eps)`; `kd = (1 - F) * (1 - metallic)`; `Lo += (kd * albedo / PI + spec) * radiance * NdotL`. Sun radiance × shadow; point radiance × range-falloff × intensity.
- Ambient: `ambient * albedo * ao` (AO modulates indirect only).
- Final: `color = ambient + Lo + emissive`. Then existing reflection mix (`mix(color, reflectColor, reflectivity)` — crude, M46 replaces) and existing fog. Output linear HDR (M44 composite tone-maps).
- The skinned vertex variant is unchanged (PBR is fragment-stage); both lit + skinned paths get PBR via the shared frag. Keep `standardLitFragSource` and `engine/render/Pbr.h` (see Testing) in lockstep.

### 6. CC0 wiring — `engine/render/TextureLoader`
Add `iron::loadMetallicRoughness(renderer, roughnessPath, metallicConstant)` → builds the combined MR texture (`G` = roughness PNG luminance, `B` = `metallicConstant`, loaded **linear**). Dielectric CC0 packs pass `0.0`; the metal pack passes `1.0`. Replaces the old `loadRoughnessAsSpec` (no inversion — PBR uses roughness directly). showcase/sandbox CC0 materials wire MR via this helper; AO omitted (scalar 1).

### 7. Test scene — `engine/scene/Mesh` + sandbox
- Add `iron::makeUVSphere(radius, segments)` → `Mesh` with positions / unit normals / tangents / UV (none exists today).
- Sandbox renders a **runtime metallic×roughness sphere grid** (NOT serialized): e.g. 6×6, metallic 0→1 across X, roughness ~0.05→1 across Z, neutral mid-grey albedo, under the sun + one point light — the canonical PBR tuning matrix. Offset from the authored scene so it doesn't interfere.

## Testing
- **Unit `tests/test_pbr.cpp`:** a CPU port of the BRDF helpers in a new header `engine/render/Pbr.h` (`distributionGGX`, `geometrySmith`, `fresnelSchlick`, `f0For(albedo, metallic)`) kept in lockstep with the GLSL. Assertions: F0 dielectric ≈ 0.04; `f0For` returns albedo at metallic=1; Fresnel→1 at grazing (cosTheta→0); GGX `D` is maximal at NdotH=1 and increases as roughness→0; geometry term in [0,1]. Plus `makeUVSphere`: expected vertex/index counts, unit-length normals, all positions at `radius` from center.
- **Visual gate (user):** sandbox sphere grid shows the classic PBR matrix (metals specular-only/tinted by albedo, dielectrics diffuse + tight bright spec, increasing roughness blurs/spreads the highlight); showcase's metal cylinder + CC0-textured props look physically plausible; existing scenes still render (dielectric defaults); no validation errors.

## Risks
- **Linear/sRGB (important):** non-color maps MUST load linear; albedo sRGB. Covered by the new color-space param (§2). Get this wrong → washed/dark or wrong normals.
- **Energy/normalization:** PBR can read dark vs Blinn-Phong; M44 HDR+tonemap + exposure helps. Tune ambient/exposure at the visual gate, not by hacking the BRDF.
- **Reflection double-count:** the kept crude reflection mix can mildly double with PBR spec; accepted until M46.
- **LitUbo/descriptor growth:** every `scenePass_` pipeline shares the layout (M43b/M45a) — consistent by construction; bump the sampler pool (§4).
- **OpenGL compile:** removing `specPower` from `Material` requires the small `OpenGLRenderer` edit (§1) or the frozen GL build breaks.

## Open questions for spec review
- Sphere-grid: always-on (corner of the sandbox) vs a toggle key? (Lean: always-on, offset from origin — simplest; revisit if intrusive.)
- `materialParams2` as a new `Vec4` vs repacking existing spare lanes? (Lean: new `Vec4` — clearest, +16 bytes is negligible.)
