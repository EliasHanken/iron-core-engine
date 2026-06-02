# M45c — Finish glTF PBR materials (design)

**Date:** 2026-06-02
**Status:** approved (brainstorm), pending implementation plan
**Track:** Render-polish detour → completes the M45 PBR work. M45a (consolidate lit shader) + M45b (Cook-Torrance GGX) merged; M45c wires glTF models' authored PBR materials into the engine.

---

## Problem

M45b made the engine PBR, but `GltfLoader` only resolves albedo + normal + metallic-roughness texture *paths* — it drops the **occlusion** + **emissive** textures and ALL the scalar **factors** (metallic/roughness/baseColor/emissive). gltf-viewer (and sandbox-loaded glTF) therefore render glTF models with scalar PBR defaults: no AO, no emissive glow, no authored metallic/roughness, no base-color tint. M45c finishes the glTF → engine-PBR path so authored materials render correctly (e.g. the DamagedHelmet's metallic visor + glowing emissive parts).

## Goals
- Extract occlusion + emissive textures and the PBR scalar factors from glTF.
- Extend the engine `Material` + shader with an **emissive map** and a **base-color factor** (the two glTF channels the engine lacks).
- Wire gltf-viewer + sandbox glTF entities to the full material.
- DamagedHelmet renders fully PBR (metal, roughness, AO, emissive, albedo tint).

## Non-goals (deferred)
- IBL / environment specular — **M46** (metals still use the crude cubemap reflection).
- glTF alpha-blend/mask modes, KHR extensions (clearcoat, transmission, emissive_strength), embedded/base64 textures (still file-URI only), multiple materials per model (first-primitive material as today).

## Decisions locked in brainstorm (2026-06-02)
- **Full scope:** include emissive texture + factor AND base-color factor (not just MR+AO).
- **Skinned bones binding:** simple bump (emissive sampler takes binding 8, bones UBO moves 8→9) — the established M45b pattern. The "move bones to set=1" refactor is deferred (noted if lighting keeps growing).
- Emissive semantics: `Material.emissive` (Vec3) is the **emissive factor**, multiplied by the emissive map (white default → backward-compatible).

---

## Architecture

### 1. `GltfLoader` (`engine/asset/GltfLoader.{h,cpp}`)
- `GltfMaterialPaths` gains `std::string occlusion;` (occlusionTexture) + `std::string emissive;` (emissiveTexture), resolved with the existing `resolve(textureIndex)` helper.
- `GltfModel` gains scalar factors read from the tinygltf material:
  - `float metallicFactor` (`mat.pbrMetallicRoughness.metallicFactor`, default 1.0)
  - `float roughnessFactor` (`mat.pbrMetallicRoughness.roughnessFactor`, default 1.0)
  - `Vec3 baseColorFactor` (`mat.pbrMetallicRoughness.baseColorFactor` rgb, default {1,1,1})
  - `Vec3 emissiveFactor` (`mat.emissiveFactor`, default {0,0,0})
  (glTF/tinygltf store these as `double`/`std::vector<double>`; cast to float.)

### 2. `Material` (`engine/render/Material.h`)
Add:
```cpp
    TextureHandle emissiveMap = kInvalidHandle;   // sRGB; multiplies `emissive`
    Vec3 baseColorFactor{1.0f, 1.0f, 1.0f};       // albedo tint (glTF baseColorFactor)
```
`emissive` (existing Vec3) is reinterpreted as the **emissive factor**: final emissive = `emissive × emissiveMap.rgb`. Defaults (`emissive={0,0,0}`, no map) → no glow; existing materials that set `emissive` directly + no map → `emissive × 1` (white default) unchanged. `baseColorFactor` default {1,1,1} → no tint (backward-compatible).

### 3. `LitUbo` + descriptor layout (`VulkanRenderer.cpp`, `VkShader.cpp`, `VkFrameRing.cpp`)
- `LitUbo`: append `Vec4 baseColorFactor;` (xyz = tint, w unused) after `materialParams2`. 944 → 960 bytes; `static_assert` updated. Mirror the field in ALL GLSL `LitUbo` blocks (lit vert, skinned vert, frag, reflection-pass vert) at the same position (std140).
- Lit descriptor layout: add **binding 8 = emissive** (COMBINED_IMAGE_SAMPLER, FRAGMENT). 8 → 9 bindings.
- Skinned descriptor layout: emissive sampler also at binding 8; the bones UBO moves **binding 8 → 9** (layout `bindingCount` 9→10, skinned vert `BoneUbo` binding 8→9, `recordSkinnedDraw` bones write `dstBinding` 8→9).
- `VkFrameRing` sampler pool: 7× → 8× `kMaxDescriptorSetsPerFrame`.
- `recordSceneDraw`: write `baseColorFactor` into the UBO; bind emissive map at binding 8 (default `whiteTexture()`); grow imgInfos/writes (lit = 9 descriptors). `recordSkinnedDraw`: same + bones at binding 9 (skinned = 10 descriptors).

### 4. Shader (`engine/render/StandardLitShader.h`, shared frag)
- Add `layout(set = 0, binding = 8) uniform sampler2D uEmissiveMap;`
- Add `vec4 baseColorFactor;` to the `LitUbo` block (after `materialParams2`).
- `vec3 albedo = texture(uDiffuse, uv).rgb * u.baseColorFactor.xyz;`
- `vec3 emissive = u.emissive.xyz * texture(uEmissiveMap, uv).rgb;` then `color = ambient + Lo + emissive;` (replaces the bare `u.emissive.xyz`).
- Skinned vert: `BoneUbo` binding 8 → 9.

### 5. Wiring
- **gltf-viewer** (`games/10-gltf-viewer/main.cpp`): from the loader's new fields, set on the render `Material`: `metallicRoughnessMap` (loadTexture linear), `aoMap` (linear), `emissiveMap` (**sRGB**), `metallic = metallicFactor`, `roughness = roughnessFactor`, `baseColorFactor`, `emissive = emissiveFactor`. (normal already linear from M45b.)
- **sandbox** (`games/11-sandbox/main.cpp`): the glTF resolve branch already sets `metallicRoughnessMap`; add `aoMap` (linear), `emissiveMap` (sRGB), and the four factors, sourced from `gltfModel`.

## sRGB / linear
- sRGB (color): albedo (baseColor), **emissive**.
- Linear (data): normal, metallic-roughness, occlusion.

## Testing
- **Unit (`tests/test_gltf_material.cpp` or extend an existing GltfLoader test):** load a glTF with a full PBR material (the `assets/.../DamagedHelmet` model ships baseColor/MR/normal/AO/emissive + factors), assert `materialPaths.occlusion`/`.emissive` resolve to non-empty paths and `metallicFactor`/`roughnessFactor`/`baseColorFactor`/`emissiveFactor` are read (helmet factors are non-default where applicable). If no GltfLoader unit test exists, add a minimal one; if the test asset isn't reachable from the test working dir, assert against a tiny inline/fixture glTF instead.
- **Visual gate (user):** gltf-viewer renders the DamagedHelmet with a metallic visor, roughness variation, AO contact-darkening, **glowing emissive** elements, and correct base-color; sandbox glTF entities likewise. No validation errors; skinned models (if any) still animate (bones-binding remap intact).

## Risks
- **sRGB/linear (recurring):** emissive must load sRGB, MR/AO linear — getting it wrong washes out or darkens.
- **Bones-binding remap (8→9):** layout + skinned vert + `recordSkinnedDraw` must agree, or skinned draws break — verify all three (the skinned visual gate confirms).
- **std140 `baseColorFactor`** must be in the same position in all four GLSL `LitUbo` blocks + the C++ struct (size 960) — a mismatch silently corrupts uniforms.
- **glTF factor defaults:** absent factors default to glTF spec values (metallic/roughness 1.0, baseColor {1,1,1}, emissive {0,0,0}); ensure the loader applies those defaults so a material with only textures still renders right.

## Open questions for spec review
- Test asset reachability: assert against the DamagedHelmet (if the test can find it) vs a tiny committed fixture glTF? (Lean: try the existing asset; fall back to a fixture only if the working-dir path is awkward.)
- Move bones to descriptor set=1 now vs simple bump? (Lean: simple bump for M45c; revisit if a future lighting feature needs another sampler.)
