# Normal + Specular Maps + Specular Lighting — Design

**Date:** 2026-05-24
**Status:** Approved, pending implementation plan
**Milestone:** Fifth on the visual/engine upgrades track (after shadow mapping,
point lights, atmosphere, reflections, materials). Adds tangent-space normal
mapping, specular maps, and a Blinn-Phong specular lighting term to the lit
render path. Strandbound gets visible wood grain on islands and a glossy
finish on the anchor pole.

## Goal

1. Surfaces look **bumpy** without adding geometry — normal maps perturb the
   shading normal per fragment via a tangent-space → world-space transform.
2. Surfaces gain **bright highlights** where they're glossy — Blinn-Phong
   specular term added to both sun and point lights, modulated per fragment
   by a specular map.
3. Strandbound's islands show plank grain via a procedural wood normal map;
   the anchor pole shows a polished-wood highlight.

## Non-goals

- **CC0 asset pack.** No external image downloads in this milestone — normal
  and specular maps are procedurally generated at startup (same approach as
  the atmosphere milestone's cubemap). Real CC0 assets land in the next
  milestone alongside the showcase scene.
- **Per-light specular tuning.** All lights use the same `uSpecPower`; no
  per-light spec multipliers.
- **PBR (roughness/metalness, GGX).** Blinn-Phong is the chosen model — one
  exponent, one greyscale spec mask, no energy conservation. PBR is a future
  milestone if visuals demand it.
- **Anisotropic / directional normals.** One tangent per vertex; isotropic
  normal maps only.
- **Parallax occlusion mapping.** No height maps.
- **Normal mapping in the reflection pass.** The simplified reflection
  shader stays simple; reflections of bumpy surfaces show flat normals.
  Acceptable visually because reflective surfaces (water) are usually
  smooth, and the reflected geometry is far enough away that fine bump
  detail wouldn't read.

## High-level architecture

Three additions:

1. **`Vertex` gains `Vec3 tangent`** — the tangent direction in world space
   (computed at mesh-build time). Bitangent is derived in the shader via
   `cross(N, T)`.
2. **`Material` gains 3 fields** — `normalMap`, `specularMap`, `specPower`.
3. **Lit shader gains TBN matrix + normal sample + Blinn-Phong spec term.**
   `uCameraPos` (already uploaded for reflections) provides the view
   direction; per-light half-vector drives the specular term.

Plus **fallback textures** on the renderer: a 1×1 "flat normal" (RGB
128,128,255) and a 1×1 "no specular" (RGB 0,0,0). The renderer binds these
when a `Material` doesn't set a normal/spec map, so the shader always has
*something* to sample at units 4 and 5.

Plus **procedural generators** in `engine/render/ProceduralTextures.h`
(header-only, mirrors the atmosphere milestone's procedural cubemap pattern):
- `std::vector<unsigned char> generateWoodNormalMap(int size, int planks)` —
  sine-wave plank-seam grooves along one axis.
- `std::vector<unsigned char> generateMetalSpecularMap(int size)` —
  uniform high-spec greyscale.

Games call these at startup to build textures for their materials.

## Per-frame data flow

Unchanged from the materials milestone. The renderer:
- Uploads `uSpecPower` per draw (`shader.setFloat("uSpecPower", call.material.specPower)`).
- Binds `uNormalMap` (sampler2D) at unit 4 — `call.material.normalMap` if set,
  else `flatNormalTexture()`.
- Binds `uSpecularMap` (sampler2D) at unit 5 — `call.material.specularMap`
  if set, else `noSpecularTexture()`.
- Lit pass loop also cleans up units 4 and 5 after rendering.

No new render passes, no signature changes.

## Vertex tangent computation

Mesh builders compute tangents at build time:

- **`appendBox`** — each face has a clear in-plane tangent direction. For
  +X/-X faces (normal along ±X), tangent is ±Z (the U axis of the face's
  UV layout). For +Y/-Y, tangent is +X. For +Z/-Z, tangent is +X (or ∓X
  depending on the existing winding — must align with the UV's U direction
  so the tangent matches "the direction of increasing U"). All 4 vertices
  of a face share the same tangent.
- **`appendQuad`** — the `u` axis already computed for UV layout becomes
  the per-vertex tangent.
- **`appendTube`** — tangent along the tube's length direction (per-ring,
  computed as the normalized direction from this ring's centre to the next
  ring's centre; the last ring uses the previous ring's direction).
- **`makeCube`** — produces a unit cube (1×1×1) via `appendBox` internally,
  so picks up the tangent computation automatically.

## Shader changes (lit shader, both Strandbound and spinning-cube)

### Vertex shader

```glsl
layout(location = 3) in vec3 aTangent;  // NEW vertex attribute
out vec3 vTangent;
// ...
vTangent = mat3(uModel) * aTangent;  // NEW
```

### Fragment shader

New uniforms:
```glsl
uniform sampler2D uNormalMap;
uniform sampler2D uSpecularMap;
uniform float uSpecPower;
in vec3 vTangent;
```

New body (additions clearly marked):
```glsl
vec3 N = normalize(vNormal);
vec3 T = normalize(vTangent);
vec3 B = cross(N, T);
mat3 TBN = mat3(T, B, N);

vec3 tangentNormal = texture(uNormalMap, vUV * uUvScale).rgb * 2.0 - 1.0;
vec3 perturbedN = normalize(TBN * tangentNormal);

vec3 V = normalize(uCameraPos - vWorldPos);  // view direction
float specMask = texture(uSpecularMap, vUV * uUvScale).r;

// Sun specular (added to existing sun lighting):
vec3 L = -normalize(uLightDir);
vec3 H = normalize(L + V);
float sunSpec = pow(max(dot(perturbedN, H), 0.0), uSpecPower);
vec3 sunSpecCol = uLightColor * sunSpec * specMask;
vec3 sunDiffuse = uLightColor * max(dot(perturbedN, L), 0.0);  // uses perturbedN now
lighting = sunDiffuse * shadowFactor() + uLightColor * uAmbient + sunSpecCol;

// Point light loop — replace existing per-light contribution with:
for (int i = 0; i < uPointLightCount; ++i) {
    vec3 toLight = uPointLights[i].position - vWorldPos;
    float dist = length(toLight);
    if (dist < 0.0001 || dist >= uPointLights[i].range) continue;
    vec3 Lp = toLight / dist;
    float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
    float diffuse = max(dot(perturbedN, Lp), 0.0);
    vec3 Hp = normalize(Lp + V);
    float spec = pow(max(dot(perturbedN, Hp), 0.0), uSpecPower);
    vec3 contribution =
        uPointLights[i].color * uPointLights[i].intensity * falloff *
        (diffuse + spec * specMask);
    lighting += contribution;
}
```

(The point-light loop replaces the existing Lambert-only loop.)

The fog `mix` and reflection composition at the end of `main` are unchanged.

### Backward compatibility

Default `Material` has `normalMap = kInvalidHandle` → renderer binds
`flatNormalTexture()` → tangent-space normal is `(0,0,1)` → TBN transforms
it to the geometric normal → `perturbedN == N`. Identical lighting to
before this milestone for any draw that doesn't opt in.

Default `specularMap = kInvalidHandle` → renderer binds `noSpecularTexture()`
→ `specMask = 0` → specular contribution is zero. Lambert-only lighting,
same as before.

`specPower = 32.0f` default — irrelevant when `specMask = 0`. When a draw
opts into a spec map, 32 is a reasonable starting glossiness; games tune
per material.

## Components and files

### New files

- `engine/render/ProceduralTextures.h` — header-only generators:
  - `std::vector<unsigned char> generateWoodNormalMap(int size, int planks)`
  - `std::vector<unsigned char> generateMetalSpecularMap(int size)`

### Modified files (engine)

- `engine/scene/Mesh.h` — `Vertex` gains `Vec3 tangent;` field (4th member
  after position/normal/uv).
- `engine/scene/Mesh.cpp`:
  - `appendBox`: compute per-face tangent (one per face, 4 verts share it).
  - `appendQuad`: store `u` axis as the tangent for all 4 verts.
  - `appendTube`: compute per-ring tangent along the tube's length.
- `engine/render/Material.h` — gain `normalMap`, `specularMap`, `specPower`.
- `engine/render/Renderer.h` — new pure-virtuals:
  - `virtual TextureHandle flatNormalTexture() const = 0;`
  - `virtual TextureHandle noSpecularTexture() const = 0;`
- `engine/render/backends/opengl/OpenGLRenderer.h/.cpp`:
  - Build `flatNormalTexture_` and `noSpecularTexture_` in the constructor
    (1×1 RGB textures via the existing `createTexture` path).
  - Implement the two `*Texture()` accessors.
  - Lit pass per-draw: upload `uSpecPower`; bind normal map to unit 4
    (fallback to `flatNormalTexture_`); bind spec map to unit 5 (fallback
    to `noSpecularTexture_`); upload sampler unit ints.
  - Lit pass cleanup: unbind units 4 and 5 after the loop.
- `engine/render/backends/opengl/GLMesh.cpp` (or wherever the vertex
  attribute setup lives) — register attribute location 3 for `vec3 aTangent`,
  matching the new `Vertex::tangent` field offset.

### Modified files (games)

- `games/01-spinning-cube/main.cpp`:
  - Vertex shader: `layout(location = 3) in vec3 aTangent; out vec3 vTangent;`
    + `vTangent = mat3(uModel) * aTangent;`.
  - Fragment shader: declare `uNormalMap`, `uSpecularMap`, `uSpecPower`,
    `in vec3 vTangent;` — body unchanged (`uReflectivity = 0` default
    means cube is still unlit textured; the new uniforms are declared so
    uploads find targets).
- `games/02-strandbound/main.cpp`:
  - Vertex shader: add tangent attribute + varying.
  - Fragment shader: full TBN + perturbed normal + Blinn-Phong spec per
    the design above. Replace the existing Lambert-only sun and point-light
    contributions with the new normal-mapped + spec versions.
  - Generate a wood normal map at startup: `generateWoodNormalMap(256, 4)`
    → `createTexture` → `woodNormalTex`.
  - Generate a metal spec map at startup: `generateMetalSpecularMap(256)`
    → `createTexture` → `metalSpecTex`.
  - Apply `woodNormalTex` to islands + prop boxes. `specPower` stays at
    default 32 (no spec map → no spec; diffuse-only with bumpy normals).
  - Anchor pole: `woodNormalTex` + `metalSpecTex` + `specPower = 64`
    (slightly polished pole gets visible highlights).
  - Water, bulbs: leave defaults.

### Not touched

- Shadow mapping pipeline, reflection pass (deliberately simplified),
  skybox pass.
- HUD, debug lines, rope physics, controller.

## Testing

- **`tests/test_mesh_builders.cpp`**: add 1 test that `appendBox` produces
  non-zero tangents on every vertex, and that the tangent is orthogonal
  to the normal for at least one face (sanity).
- **`tests/test_material.cpp`** (or fold into `test_reflection`): assert
  the new `Material` defaults — `normalMap = kInvalidHandle`,
  `specularMap = kInvalidHandle`, `specPower = 32.0f`.
- **Manual playtest:**
  - Home island floor shows visible plank-seam grooves (the normal map
    catches light direction).
  - Anchor pole shows a bright highlight that moves as you walk around it.
  - Lantern bulbs unchanged (still emissive, no normal/spec).
  - Sun shadows + reflections still work.
  - Spinning-cube unchanged.

## Constants

```cpp
// engine/render/backends/opengl/OpenGLRenderer.cpp internal:
constexpr int kFlatNormalSize = 1;          // 1x1 fallback
constexpr int kNoSpecularSize = 1;
constexpr unsigned char kFlatNormalRGB[3] = {128, 128, 255};
constexpr unsigned char kNoSpecularRGB[3] = {0, 0, 0};
```

## Out of scope (carried forward)

- **CC0 asset pack + showcase scene** — next milestone. By then the engine
  has every visual feature; the showcase is composition + asset sourcing.
- **PBR (roughness/metalness)** — future, if the demos call for it.
- **Parallax mapping, displacement mapping** — future.
- **Per-light spec power** — future.

## References

- Previous milestone: materials + UV scale
  (`docs/superpowers/specs/2026-05-23-materials-and-uv-scale-design.md`).
- Procedural-texture pattern established in the atmosphere milestone
  (sunset cubemap generated at startup) — reused here for normal and
  spec maps.
- Blinn-Phong is the classic "shiny without being fancy" lighting model;
  fits the engine's "clear, learnable, hand-rolled" philosophy.
