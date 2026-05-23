# Materials & UV Scale Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Introduce `Material` struct (bundles existing `DrawCall` material
fields + new `uvScale`); fix `appendBox`/`appendQuad` UV-stretch bug; retile
Strandbound's wood textures cleanly.

**Architecture:** `Material` is plain data embedded by value on `DrawCall`. The
five existing fields (`texture`, `emissive`, `reflectivity`,
`useReflectionPlane`) plus new `uvScale` move into the struct. Mesh builders
write world-space-extent UVs (so a 20×1 face gets `(0,0)→(20,1)` UVs);
combined with GL_REPEAT and the shader's `vUV * uUvScale`, this means
textures tile naturally on large faces instead of stretching.

**Tech Stack:** C++23, OpenGL 3.3, existing RHI.

**Spec:** `docs/superpowers/specs/2026-05-23-materials-and-uv-scale-design.md`.

**Task order rationale:** 4 tasks. Task 1 is a big atomic refactor (every
DrawCall site updates in lockstep). Task 2 is the UV behaviour change in
mesh builders — visually changes Strandbound (tiles instead of stretches).
Task 3 adds the `uvScale` parameterization. Task 4 docs.

---

## Task 1: Material struct + DrawCall migration

**Files:**
- Create: `engine/render/Material.h`
- Modify: `engine/render/Renderer.h` (DrawCall restructured)
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp` (per-draw reads use `call.material.*`)
- Modify: `games/01-spinning-cube/main.cpp` (DrawCall construction)
- Modify: `games/02-strandbound/main.cpp` (every DrawCall construction)
- Modify: `games/02-strandbound/RopeTool.cpp` (rope DrawCall construction)

Atomic refactor. Bundles five existing fields into a `Material` struct.

- [ ] **Step 1: Create `engine/render/Material.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "render/Handles.h"

namespace iron {

// Per-surface material: how a draw should sample its texture, glow, reflect.
// Embedded by value on DrawCall. Defaults match the previous bare-DrawCall
// defaults (texture invalid, no glow, matte, no planar reflection, no
// UV tiling change).
struct Material {
    TextureHandle texture = kInvalidHandle;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
    bool useReflectionPlane = false;
    float uvScale = 1.0f;  // multiplies sampled UV; >1 = tile more times
};

} // namespace iron
```

- [ ] **Step 2: Restructure DrawCall in Renderer.h**

In `engine/render/Renderer.h`, add `#include "render/Material.h"`. Replace the
existing `DrawCall` definition (which currently has the five bare fields):

```cpp
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Material material{};
};
```

(Remove the old `texture`, `emissive`, `reflectivity`, `useReflectionPlane`
fields — they live in `material` now.)

- [ ] **Step 3: Update OpenGLRenderer.cpp's per-draw reads**

In `engine/render/backends/opengl/OpenGLRenderer.cpp`:
- **Lit pass loop:** rewrite per-draw reads from `call.texture` to
  `call.material.texture`, `call.emissive` to `call.material.emissive`,
  `call.reflectivity` to `call.material.reflectivity`,
  `call.useReflectionPlane` to `call.material.useReflectionPlane`.
- **Reflection pass loop:** rewrite the `call.useReflectionPlane` check
  and the `call.texture` read similarly.

Don't add the `uvScale` uniform upload yet — Task 3 does that.

- [ ] **Step 4: Update spinning-cube DrawCall construction**

In `games/01-spinning-cube/main.cpp`, find the DrawCall construction. Replace
`dc.texture = ...` (and anything else referencing the old bare fields) with
`dc.material.texture = ...`. The cube only sets `texture` today, so this is
one line.

- [ ] **Step 5: Update Strandbound DrawCall constructions**

In `games/02-strandbound/main.cpp`, find every DrawCall construction:
- World object draws (boxes / anchor pole): set `dc.material.texture`,
  `dc.material.reflectivity`, `dc.material.useReflectionPlane`.
- Bulb draws: `dc.material.texture`, `dc.material.emissive`.
- Water draw: `dc.material.texture`, `dc.material.reflectivity = 0.85f`,
  `dc.material.useReflectionPlane = true`.

If a sub-struct designator like `dc.material = Material{...}` is cleaner at
some sites, use it. Either style works.

- [ ] **Step 6: Update RopeTool.cpp DrawCall**

In `games/02-strandbound/RopeTool.cpp`, find the rope `DrawCall` build site.
Same migration: `ropeCall.material.texture = ...`, `ropeCall.material.reflectivity = 0.08f`, etc.

- [ ] **Step 7: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 tests pass. **Visually identical** to before —
this is a pure refactor.

- [ ] **Step 8: Commit**

```powershell
git add engine/render/Material.h engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp games/02-strandbound/main.cpp games/02-strandbound/RopeTool.cpp
git commit -m @'
Introduce Material struct; collapse DrawCall material fields

DrawCall now carries a single Material material; instead of five
bare fields (texture, emissive, reflectivity, useReflectionPlane).
Defaults match the previous defaults so behaviour is unchanged.
uvScale is a new Material field (default 1.0); it's used by the
mesh-builder and shader changes in subsequent commits.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, verify `git log -1 --pretty=%s` has no stray `@`.

---

## Task 2: appendBox / appendQuad world-space UVs + test updates

**Files:**
- Modify: `engine/scene/Mesh.cpp` (UV generation in appendBox, appendQuad)
- Modify: `tests/test_mesh_builders.cpp` (update existing UV tests; add 1 new)

`appendBox` and `appendQuad` currently write UVs spanning `0..1` regardless of
face size. After this task: UVs span `(0,0)→(faceWidth, faceHeight)` measured
in world units. Combined with GL_REPEAT (which textures already use) and the
shader's `vUV * uUvScale` (added in Task 3), this means a 20×1 face shows the
texture tiled 20 times horizontally and once vertically instead of stretched.

**Important caveat: Strandbound visuals change as soon as this lands** — the
home island's wood texture will start tiling at 20×20 reps. The shader doesn't
multiply by `uvScale` yet (Task 3), but that's fine because `uvScale = 1.0`
default + GL_REPEAT means "1 texture per world unit" which is the natural
density.

- [ ] **Step 1: Update `appendBox` UV generation**

In `engine/scene/Mesh.cpp`, find `appendBox`. Each of the 6 faces currently
emits 4 vertices with UVs `(0,0)`, `(1,0)`, `(1,1)`, `(0,1)`. For each face,
compute the face's two in-plane dimensions from `size` and use those as the
UV max instead of `1.0`:

- `+X` / `-X` faces (perpendicular to X): UV spans `(0,0)→(size.z, size.y)`.
- `+Y` / `-Y` faces (perpendicular to Y): UV spans `(0,0)→(size.x, size.z)`.
- `+Z` / `-Z` faces (perpendicular to Z): UV spans `(0,0)→(size.x, size.y)`.

The exact mapping depends on how the existing code lays out the face quads —
read the existing function carefully and adapt. The key change: where the
old code wrote `Vec2{1.0f, 0.0f}`, write `Vec2{u_extent, 0.0f}`. Similarly
for `(1,1)` and `(0,1)`.

- [ ] **Step 2: Update `appendQuad` UV generation**

In `engine/scene/Mesh.cpp`, find `appendQuad`. Current vertex pushes are:
```cpp
out.vertices.push_back(Vertex{p0, normal, Vec2{0.0f, 0.0f}});
out.vertices.push_back(Vertex{p1, normal, Vec2{1.0f, 0.0f}});
out.vertices.push_back(Vertex{p2, normal, Vec2{1.0f, 1.0f}});
out.vertices.push_back(Vertex{p3, normal, Vec2{0.0f, 1.0f}});
```

Change to:
```cpp
out.vertices.push_back(Vertex{p0, normal, Vec2{0.0f, 0.0f}});
out.vertices.push_back(Vertex{p1, normal, Vec2{size.x, 0.0f}});
out.vertices.push_back(Vertex{p2, normal, Vec2{size.x, size.y}});
out.vertices.push_back(Vertex{p3, normal, Vec2{0.0f, size.y}});
```

- [ ] **Step 3: Update existing test assertions**

In `tests/test_mesh_builders.cpp`:
- The existing `appendBox` tests likely don't directly assert UV values — verify.
- The existing `appendQuad` tests (Task 2 of reflections milestone) test
  vertex/index counts, accumulation, and spatial extents. They DON'T assert
  on UVs directly. So they should still pass.

If any test relies on `0..1` UVs, update the assertion to expect world-space
extents.

- [ ] **Step 4: Add a new appendBox UV test**

Add ONE test in `test_mesh_builders.cpp`:

```cpp
// appendBox now writes world-space-extent UVs (so textures tile).
{
    MeshData m;
    appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 2.0f, 1.0f});
    // Find the max U and max V across all 24 vertices.
    float maxU = 0.0f, maxV = 0.0f;
    for (const auto& v : m.vertices) {
        if (v.uv.x > maxU) maxU = v.uv.x;
        if (v.uv.y > maxV) maxV = v.uv.y;
    }
    // Largest face is the +X or -X face (perpendicular to X): UV spans
    // (size.z, size.y) = (1, 2). Largest +Y/-Y: (size.x, size.z) = (4, 1).
    // Largest +Z/-Z: (size.x, size.y) = (4, 2). So max U should be 4, max V
    // should be 2.
    CHECK_NEAR(maxU, 4.0f);
    CHECK_NEAR(maxV, 2.0f);
}
```

(Adapt the comment to match the actual UV mapping in your implementation
if the per-face axis assignment differs slightly.)

- [ ] **Step 5: Build, test, playtest unavailable so just verify build is clean**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 tests pass. **Strandbound visuals change** —
wood textures now tile.

- [ ] **Step 6: Commit**

```powershell
git add engine/scene/Mesh.cpp tests/test_mesh_builders.cpp
git commit -m @'
appendBox/appendQuad emit world-space-extent UVs

Face UVs span (0,0) to (faceWidth, faceHeight) in world units
instead of 0..1. Combined with GL_REPEAT (which existing textures
already use), this means a 20x1 face tiles its texture 20 times
horizontally and once vertically. Fixes the long-standing
texture-stretch bug on Strandbound's islands and anchor pole.

The uUvScale uniform that lets games dial tile density per draw
lands in the next commit.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, verify subject.

---

## Task 3: uvScale uniform + shader changes + Strandbound uvScale per-surface

**Files:**
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp` (upload `uUvScale`)
- Modify: `games/01-spinning-cube/main.cpp` (shader declares + applies)
- Modify: `games/02-strandbound/main.cpp` (shader declares + applies; per-DrawCall uvScale)

- [ ] **Step 1: Upload `uUvScale` in the lit pass**

In `engine/render/backends/opengl/OpenGLRenderer.cpp`'s lit-pass per-draw
loop, add (alongside the existing `setFloat("uReflectivity", ...)` line or
similar):

```cpp
shader.setFloat("uUvScale", call.material.uvScale);
```

The reflection pass shader is deliberately simplified and doesn't sample
the texture differently per material — it can use `uvScale = 1.0` for now.
If we add `uvScale` to the reflection pass later it's a one-line addition.

- [ ] **Step 2: Update Strandbound's fragment shader**

In `games/02-strandbound/main.cpp`'s `kFragmentShader`:
- Add `uniform float uUvScale;` alongside the other uniforms.
- Find `vec4 texel = texture(uTexture, vUV);` and replace with:
  ```glsl
  vec4 texel = texture(uTexture, vUV * uUvScale);
  ```

- [ ] **Step 3: Update spinning-cube's fragment shader**

Same change in `games/01-spinning-cube/main.cpp`'s `kFragmentShader`. Add
the uniform declaration and apply `vUV * uUvScale` in the texture sample.
The cube's mesh has 0..1 UVs from `makeCube` (unchanged by Task 2 because
the cube is 1×1×1 so world-space extents = 0..1), and `uvScale = 1.0` is
the default — visually identical.

- [ ] **Step 4: Set per-surface `uvScale` in Strandbound**

In `games/02-strandbound/main.cpp`:
- **Water plane DrawCall:** set `dc.material.uvScale = 0.0f` so the white
  texture is sampled at a single texel (`whiteTexture` is 1×1 white so
  any sample is white, but explicit 0 documents the intent).
- **All other DrawCalls** (boxes, anchor pole, bulbs, ropes): leave at
  default `uvScale = 1.0`. The world-space-extent UVs from Task 2 mean
  this is "1 texture per world unit" which fits the wood-plank aesthetic.

- [ ] **Step 5: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 tests pass.

- [ ] **Step 6: Commit**

```powershell
git add engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp games/02-strandbound/main.cpp
git commit -m @'
Apply uUvScale in lit fragment shader

Per-draw uniform multiplies the sampled UV before texture lookup.
uvScale=1.0 (the default) gives "one texture per world unit"
density via the world-space-extent UVs from the previous commit.
uvScale=0 collapses to single-texel sampling (used for the
Strandbound water plane). Spinning-cube unchanged.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, verify subject.

---

## Task 4: Update lighting docs

**File:** `docs/engine/lighting.md`

Append a `## Materials` section. Insert AFTER the existing `## Reflections`
section and BEFORE `## Normals and scaling`.

- [ ] **Step 1: Read the doc and insert the section**

Section content:

```markdown
## Materials

Each draw carries a `Material`: the texture, emissive colour,
reflectivity, planar-reflection flag, and a `uvScale` multiplier on
sampled UVs. Bundling these into one struct keeps the surface contract
in one place and makes future additions (normal maps, specular maps,
roughness) clean to add.

**UV tiling.** Mesh builders (`appendBox`, `appendQuad`) write
world-space-extent UVs — a 20×1 face emits `(0,0)→(20,1)` UVs, not
`0..1`. Combined with `GL_REPEAT` wrap and the shader's
`texture(uTexture, vUV * uUvScale)`, textures tile naturally on large
faces. `uvScale = 1.0` (the default) means "one texture per world unit";
`uvScale = 2.0` means "one per half-unit" (denser); `uvScale = 0.0`
collapses to single-texel sampling for "flat colour" surfaces like
water with a 1×1 white texture.

A future milestone adds **normal maps** (perturbed surface normals for
fine detail without geometry) and **specular maps + a specular lighting
term** (bright highlights from glossy patches). Both naturally live on
`Material`.
```

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/lighting.md
git commit -m @'
Document Material struct and UV tiling

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, verify subject.

---

## Self-review checklist (the planner's)

- [ ] **Spec coverage:**
  - `Material` struct → Task 1 ✓
  - `DrawCall` migration → Task 1 ✓
  - `appendBox`/`appendQuad` UV change → Task 2 ✓
  - UV test updates → Task 2 ✓
  - `uUvScale` uniform upload → Task 3 ✓
  - Lit shader uvScale multiply → Task 3 ✓
  - Spinning-cube uniform decl → Task 3 ✓
  - Strandbound per-surface uvScale → Task 3 ✓
  - Water uvScale=0 → Task 3 ✓
  - Doc update → Task 4 ✓

- [ ] **No placeholders.** ✓

- [ ] **Type consistency:** `Material` field names (`texture`, `emissive`,
  `reflectivity`, `useReflectionPlane`, `uvScale`) consistent across Task 1
  (definition), Task 1 (DrawCall + caller updates), Task 3 (per-draw upload),
  Task 3 (Strandbound `dc.material.uvScale = 0.0f`). `uUvScale` uniform name
  consistent across both shader files. ✓

- [ ] **The reflection pass uses the simplified shader** (no `uUvScale`,
  no specular, no shadows). Task 3 explicitly notes this so the implementer
  doesn't accidentally extend it.

- [ ] **Pre-flagged risk:** Strandbound's visuals change at end of Task 2
  (tiling kicks in). Acceptable — that's the intended fix.
