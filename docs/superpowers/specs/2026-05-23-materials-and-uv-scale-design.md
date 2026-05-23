# Materials & UV Scale — Design

**Date:** 2026-05-23
**Status:** Approved, pending implementation plan
**Milestone:** Fourth on the visual/engine upgrades track (after shadow mapping,
point lights, atmosphere, reflections). Introduces a `Material` struct that
collapses the existing per-`DrawCall` material fields, fixes the long-standing
`appendBox` UV-stretching bug via a new `uvScale`, and re-tiles Strandbound's
wood textures cleanly.

## Goal

1. Introduce `Material` — bundles `texture`, `emissive`, `reflectivity`,
   `useReflectionPlane`, plus new `uvScale`. `DrawCall` carries one
   `Material material` instead of five separate fields.
2. Fix `appendBox` (and `appendQuad`) so face UVs span the face's
   world-space dimensions instead of always `0..1`. Combined with
   `uvScale`, this means textures tile naturally on large faces instead
   of stretching.
3. Strandbound: wood texture finally tiles correctly on the home island,
   far island, and anchor pole.

## Non-goals

- **Normal maps.** Needs tangents on `Vertex` + TBN matrix in shader.
  Next milestone.
- **Specular maps + specular lighting term.** Needs the lit shader to
  add a specular term first (Blinn-Phong / spec-power decision). Next
  milestone.
- **Roughness, metalness, PBR.** Future milestone.
- **MaterialHandle / material resource table.** `Material` is embedded
  by value on `DrawCall`. A handle-based system can come later if games
  start sharing many materials and we want to avoid the per-draw copy.
- **Texture atlasing or UV offset.** Just scale; no offset.
- **Non-square texture tiling.** Single `float uvScale` applies to both
  U and V. A `Vec2 uvScale` can come if/when we have a texture that's
  not square.

## High-level architecture

`Material` is plain data, embedded by value on `DrawCall`:

```cpp
// engine/render/Material.h
struct Material {
    TextureHandle texture = kInvalidHandle;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
    bool useReflectionPlane = false;
    float uvScale = 1.0f;  // multiplies sampled UV; >1 = tile more times
};
```

`DrawCall` becomes:

```cpp
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Material material{};
};
```

The renderer reads `call.material.texture`, `call.material.emissive`,
etc. — same uniform uploads as today, just sourced from one nested struct
instead of bare fields.

**UV semantics change** in mesh builders. Currently `appendBox` writes
`(0,0)→(1,1)` UVs per face; after this milestone, UVs span
`(0,0)→(faceWidth, faceHeight)` measured in world units. A 20×1 face
gets UVs spanning 0..20 in U and 0..1 in V — the texture tiles 20 times
horizontally and once vertically. Combined with `uvScale`, the game can
dial in tile density per surface.

**Shader change:** `texture(uTexture, vUV * uUvScale)` (lit shader and
reflection-pass shader). When `uvScale = 1.0`, world-space-extent UVs
mean "1 texture per world unit." When `uvScale = 0.5`, "1 texture per 2
world units." When `uvScale = 0`, the texture sample is the texel at
`(0, 0)` — useful for "flat colour, no tiling" surfaces.

## Per-frame data flow

Identical to today except: per draw, the lit pass uploads
`uUvScale = call.material.uvScale` and reads other fields off
`call.material.*` instead of `call.*`. No new render passes, no new
uniform buffers, no signature changes.

## Components and files

### New files

- `engine/render/Material.h` — the `Material` struct.

### Modified files (engine)

- `engine/render/Renderer.h` — `DrawCall` restructured. The five fields
  `texture`, `emissive`, `reflectivity`, `useReflectionPlane`, plus
  the new `uvScale`, move into a nested `Material material` field.
- `engine/render/backends/opengl/OpenGLRenderer.cpp`:
  - Lit pass per-draw reads `call.material.texture`,
    `call.material.emissive`, `call.material.reflectivity`,
    `call.material.useReflectionPlane`.
  - New per-draw `shader.setFloat("uUvScale", call.material.uvScale)`.
  - Reflection pass per-draw reads `call.material.texture` (same field
    just moved).
- `engine/scene/Mesh.cpp`:
  - `appendBox`: face UVs become `(0,0)→(width, height)` in world units
    instead of `0..1`. The width/height are derived from the face's two
    in-plane size components.
  - `appendQuad`: UVs become `(0,0)→(size.x, size.y)` instead of `0..1`.

### Modified files (games)

- `games/01-spinning-cube/main.cpp`:
  - Fragment shader: `vec4 texel = texture(uTexture, vUV * uUvScale);`
    (replace the existing `texture(uTexture, vUV)` calls). Declare
    `uniform float uUvScale;`.
  - DrawCall construction: move `texture` into `material.texture`.
    Default `uvScale = 1.0f` means the cube's UVs (which are already
    `0..1` from the cube mesh) sample as before. Visually identical.
- `games/02-strandbound/main.cpp`:
  - Fragment shader: `texture(uTexture, vUV * uUvScale)` (one line
    change). Declare `uniform float uUvScale;`.
  - Every DrawCall construction site: rebuild via `Material{}` and
    `material.texture = …; material.emissive = …; material.reflectivity = …;
    material.useReflectionPlane = …; material.uvScale = …;` (or
    designated-initialiser-style aggregate init).
  - **Water plane**: set `material.uvScale = 0.0f` so the white texture
    samples the same texel everywhere (no tiling needed; the reflection
    sample dominates anyway).
  - **Land surfaces**: `material.uvScale = 1.0f` — combined with the
    world-space-extent UVs from `appendBox`, this gives "one texture per
    world unit" which fits the wood-plank aesthetic naturally.
- `games/02-strandbound/RopeTool.cpp`:
  - Rope DrawCall construction: same restructuring.
- `tests/test_mesh_builders.cpp`:
  - Update existing UV-related assertions to expect world-space-extent
    UVs from `appendBox` and `appendQuad`. Add one assertion for a
    non-unit-cube case (e.g. `appendBox({0,0,0}, {4,2,1})` → top face UVs
    span `(0,0)→(4,1)`).

### Not touched

- Shadow mapping pipeline, point-light upload, fog uniforms, skybox
  pass, reflection pass machinery (only the per-draw texture read site
  changes).
- HUD, debug lines, rope physics, controller, raycasting.

## Default-compatibility

Every existing `DrawCall` site collapses cleanly:
- `dc.texture = T` → `dc.material.texture = T`.
- `dc.emissive = E` → `dc.material.emissive = E`.
- `dc.reflectivity = R` → `dc.material.reflectivity = R`.
- `dc.useReflectionPlane = B` → `dc.material.useReflectionPlane = B`.
- New `dc.material.uvScale = 1.0f` is the default.

Combined with the mesh-builder UV change: a 1×1×1 cube's face UVs go
from `0..1` to `0..1` (unchanged — width and height are both 1). The
spinning-cube demo (which uses `makeCube`) is visually unchanged. The
Strandbound boxes change visibly (textures tile instead of stretch),
which is the intended fix.

## Error handling and edge cases

- **`uvScale = 0`:** the texture sample collapses to the texel at
  `(0, 0)` for every fragment. Useful for the water plane and for
  any "flat colour" surface. No NaN.
- **Negative `uvScale`:** flips the texture; harmless, sampler wraps
  via GL_REPEAT (or clamps if the texture's wrap mode is changed
  later).
- **Very large `uvScale`:** the texture tiles many times per face;
  GL_REPEAT handles it. Aliasing may appear at distance — that's a
  mipmapping/anisotropic-filtering problem for a future milestone.
- **`DrawCall::material.texture = kInvalidHandle`:** falls back to
  `fallbackTexture_` as today (the existing code path is unchanged;
  just sourced from one extra struct hop).

## Testing

- **Update `test_mesh_builders.cpp`:** the existing tests assume
  `0..1` UVs. Adjust the assertions to expect world-space extents.
  Add ONE new assertion for a non-unit box (e.g.
  `appendBox(center=0, size={4,2,1})` → top face UV max is `(4,1)`).
- **New 2-test `test_material.cpp`** (or fold into an existing test
  file): default `Material{}` matches the previous bare-`DrawCall`
  defaults — texture invalid, emissive zero, reflectivity zero,
  useReflectionPlane false, uvScale 1.0.
- **Manual playtest checklist:**
  - Home island's wood texture now shows many tiled planks instead of
    one giant stretched plank.
  - Far island the same.
  - Anchor pole shows wood tiling vertically (4 repetitions) and
    sideways (~0.4 repetitions, so visually about half a plank).
  - Water still solid (no tiling artifact — `uvScale = 0` works).
  - Spinning-cube unchanged.
  - All previous features still work: shadows, point lights, fog,
    skybox, reflections.
  - No GL errors.

## Out of scope (next milestone)

- **Normal maps.** `Vertex` needs `Vec3 tangent` (bitangent derivable
  via `cross(normal, tangent)`). Mesh builders compute tangents.
  `Material` gains `TextureHandle normalMap`. Lit shader builds TBN
  matrix and samples normalMap to perturb the normal before lighting.
- **Specular maps + specular lighting term.** Lit shader adds Blinn-
  Phong specular (`pow(max(dot(reflectDir, viewDir), 0), spec)`).
  `Material` gains `TextureHandle specularMap` (greyscale, scales the
  specular contribution per fragment) and `float specPower`.
- A combined "lighting detail" milestone covers both above as one
  cohesive ship — they share the lit-shader edit point.

## References

- Previous milestone: reflections
  (`docs/superpowers/specs/2026-05-23-reflections-design.md`).
- The texture-stretching bug was flagged in PR #14 review and in the
  visuals-track roadmap as a real defect worth fixing on its own.
- Reviewers in PR #12 and PR #14 flagged the bare-field `DrawCall` as a
  natural moment to introduce `Material`.
