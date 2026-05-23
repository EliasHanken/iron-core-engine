# Normal + Specular Maps + Specular Lighting Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to
> implement this plan task-by-task.

**Goal:** Add tangent-space normal mapping, specular maps, and a Blinn-Phong
specular lighting term. Strandbound gets visible wood grain on islands and
a polished highlight on the anchor pole.

**Architecture:** `Vertex::tangent` (computed by mesh builders) +
`Material::{normalMap, specularMap, specPower}` + TBN matrix and Blinn-Phong
in the lit shader. Procedural wood-normal and metal-spec generators in
`engine/render/ProceduralTextures.h`. Fallback 1×1 textures (flat normal +
black spec) on the renderer so default `Material{}` is identical to today.

**Tech Stack:** C++23, OpenGL 3.3, existing RHI.

**Spec:** `docs/superpowers/specs/2026-05-24-normal-and-specular-maps-design.md`.

**Task count:** 5 (compressed per "code quicker" cadence).

---

## Task 1: Vertex::tangent + mesh-builder tangents + test

**Files:**
- Modify: `engine/scene/Mesh.h` (add `Vec3 tangent` to `Vertex`)
- Modify: `engine/scene/Mesh.cpp` (`appendBox`, `appendQuad`, `appendTube`)
- Modify: `engine/render/backends/opengl/GLMesh.cpp` (register attribute 3)
- Modify: `tests/test_mesh_builders.cpp` (1 new tangent test)

Mesh builders compute per-face/per-ring tangents. GL backend registers the
new attribute location. Pure addition — shaders still work because they
don't read `aTangent` yet (next task adds the shader-side change for
Strandbound; spinning-cube's shader is updated in Task 3).

### Step 1 — Add `tangent` to `Vertex`

In `engine/scene/Mesh.h`, find the `Vertex` struct. Add `Vec3 tangent` as
the 4th field (after position/normal/uv):

```cpp
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec3 tangent;  // NEW
};
```

### Step 2 — Update `appendBox` to compute per-face tangent

In `engine/scene/Mesh.cpp`'s `appendBox`, each face needs a tangent
direction matching its UV's U axis. The simplest mapping that matches the
existing UV layout (where U increases along one in-plane axis):

- `+X` face (normal=+X): tangent = `+Z` (matches U-axis of UV layout for +X face).
- `-X` face (normal=-X): tangent = `-Z`.
- `+Y` face (normal=+Y): tangent = `+X`.
- `-Y` face (normal=-Y): tangent = `+X`.
- `+Z` face (normal=+Z): tangent = `+X`.
- `-Z` face (normal=-Z): tangent = `-X`.

**Important:** the exact tangent direction must match the U-axis of the
existing UV layout — read the existing UV writes for each face and pick
the tangent that points "in the direction of increasing U." If the existing
code's face layout differs from the table above, adapt.

Add a `faceTangents[6]` array next to the existing `faces[]` and emit each
vertex with the matching tangent:

```cpp
const Vec3 faceTangents[6] = {
    Vec3{0.0f, 0.0f, 1.0f},   // +X
    Vec3{0.0f, 0.0f, -1.0f},  // -X
    Vec3{1.0f, 0.0f, 0.0f},   // +Y
    Vec3{1.0f, 0.0f, 0.0f},   // -Y
    Vec3{1.0f, 0.0f, 0.0f},   // +Z
    Vec3{-1.0f, 0.0f, 0.0f},  // -Z
};
// In the face-emission loop, pass faceTangents[faceIdx] as Vertex::tangent.
```

### Step 3 — Update `appendQuad` to use the existing `u` axis

In `appendQuad`, the local `u` Vec3 is already computed for UV layout
(after the Gram-Schmidt projection). Reuse it as the tangent for all 4
vertices:

```cpp
out.vertices.push_back(Vertex{p0, normal, Vec2{0.0f, 0.0f}, u});
out.vertices.push_back(Vertex{p1, normal, Vec2{size.x, 0.0f}, u});
out.vertices.push_back(Vertex{p2, normal, Vec2{size.x, size.y}, u});
out.vertices.push_back(Vertex{p3, normal, Vec2{0.0f, size.y}, u});
```

### Step 4 — Update `appendTube` per-ring tangent

In `appendTube`, the tangent is the direction along the tube's length —
from this ring's centre to the next ring's centre, normalised. For the
last ring, reuse the previous ring's tangent. Read the existing code and
adapt; emit the tangent on each vertex of each ring.

If `appendTube`'s structure makes per-ring tangent computation awkward,
the implementer should adapt — the key requirement is "every vertex has a
non-zero tangent vector."

### Step 5 — Register the new attribute in GLMesh

In `engine/render/backends/opengl/GLMesh.cpp`, find where vertex attribute
pointers are set up (likely `glVertexAttribPointer` + `glEnableVertexAttribArray`
for attributes 0, 1, 2). Add attribute 3 for `aTangent`:

```cpp
glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                      reinterpret_cast<void*>(offsetof(Vertex, tangent)));
glEnableVertexAttribArray(3);
```

(Match the exact pattern of attributes 0/1/2 in the file.)

### Step 6 — Add 1 tangent test

In `tests/test_mesh_builders.cpp`, add at the end (near the other
`appendBox` tests):

```cpp
// appendBox produces non-zero tangents on every vertex, orthogonal to normal.
{
    MeshData m;
    appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f});
    for (const auto& v : m.vertices) {
        const float tlen = std::sqrt(v.tangent.x*v.tangent.x +
                                      v.tangent.y*v.tangent.y +
                                      v.tangent.z*v.tangent.z);
        CHECK(tlen > 0.5f);  // non-zero, near unit length
        // Tangent should be orthogonal to normal.
        const float dotTN = v.tangent.x*v.normal.x +
                            v.tangent.y*v.normal.y +
                            v.tangent.z*v.normal.z;
        CHECK_NEAR(dotTN, 0.0f);
    }
}
```

Add `#include <cmath>` if not present.

### Step 7 — Build + test

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 tests pass. **Visually identical** (shaders
don't sample `aTangent` yet).

### Step 8 — Commit

```powershell
git add engine/scene/Mesh.h engine/scene/Mesh.cpp engine/render/backends/opengl/GLMesh.cpp tests/test_mesh_builders.cpp
git commit -m @'
Add Vertex::tangent and compute tangents in mesh builders

Vertex gains Vec3 tangent (the in-plane direction matching the
U axis of the face's UV layout). appendBox emits per-face
tangents; appendQuad reuses its u axis; appendTube uses the
along-length direction. GL backend registers attribute 3 for
aTangent. New test asserts non-zero tangents orthogonal to
normals on every appendBox vertex.

No shader change yet — shaders pick up aTangent in the next
commit. Visually identical.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

Verify `git log -1 --pretty=%s` has no stray `@`.

---

## Task 2: Material + fallback textures on Renderer

**Files:**
- Modify: `engine/render/Material.h` (3 new fields)
- Modify: `engine/render/Renderer.h` (2 new pure-virtuals)
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h/.cpp` (members + impls)

### Step 1 — Add Material fields

In `engine/render/Material.h`:

```cpp
struct Material {
    TextureHandle texture = kInvalidHandle;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
    bool useReflectionPlane = false;
    float uvScale = 1.0f;
    // NEW:
    TextureHandle normalMap = kInvalidHandle;
    TextureHandle specularMap = kInvalidHandle;
    float specPower = 32.0f;
};
```

### Step 2 — Add fallback texture virtuals to Renderer

In `engine/render/Renderer.h`, alongside `whiteTexture()`:

```cpp
// A built-in 1x1 "flat normal" texture (RGB 128,128,255 = +Z in tangent
// space). Bound to the normal-map sampler when a draw's material doesn't
// set normalMap, so the shader's TBN sample returns the geometric normal
// unchanged.
virtual TextureHandle flatNormalTexture() const = 0;

// A built-in 1x1 "no specular" texture (RGB 0,0,0). Bound when a draw's
// material doesn't set specularMap, so the shader's spec contribution
// is zero.
virtual TextureHandle noSpecularTexture() const = 0;
```

### Step 3 — Implement in OpenGLRenderer

In `engine/render/backends/opengl/OpenGLRenderer.h`, add:

```cpp
TextureHandle flatNormalTexture() const override;
TextureHandle noSpecularTexture() const override;
```

Add private members:

```cpp
TextureHandle flatNormalTexture_ = kInvalidHandle;
TextureHandle noSpecularTexture_ = kInvalidHandle;
```

In `OpenGLRenderer.cpp` constructor, after `whiteTexture_` is initialised
(via the existing pattern — likely `createTexture(1, 1, white_pixel)`),
add:

```cpp
{
    const unsigned char flatNormalPixels[4] = {128, 128, 255, 255};
    flatNormalTexture_ = createTexture(1, 1, flatNormalPixels);
}
{
    const unsigned char noSpecPixels[4] = {0, 0, 0, 255};
    noSpecularTexture_ = createTexture(1, 1, noSpecPixels);
}
```

(Match the surrounding initialisation style. The pixel format is RGBA
because `createTexture` takes RGBA — pad with 255 alpha.)

Add the accessor implementations:

```cpp
TextureHandle OpenGLRenderer::flatNormalTexture() const {
    return flatNormalTexture_;
}

TextureHandle OpenGLRenderer::noSpecularTexture() const {
    return noSpecularTexture_;
}
```

### Step 4 — Build + test

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 pass. Nothing reads the new fields/textures
yet.

### Step 5 — Commit

```powershell
git add engine/render/Material.h engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp
git commit -m @'
Add Material normal/spec fields and renderer fallback textures

Material gains normalMap (TextureHandle), specularMap
(TextureHandle), specPower (float, default 32). Renderer gains
flatNormalTexture() and noSpecularTexture() — 1x1 RGB fallbacks
used by the lit pass when a draw doesn't set its own maps.

The lit shader and uniform uploads land in the next commit.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 3: Lit shader TBN + Blinn-Phong + uniform uploads + spinning-cube

**Files:**
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp` (uniform uploads + texture bindings)
- Modify: `games/01-spinning-cube/main.cpp` (vertex + fragment shader uniforms)
- Modify: `games/02-strandbound/main.cpp` (vertex + fragment shader: TBN + Blinn-Phong)

### Step 1 — Strandbound vertex shader: tangent attribute + varying

In `games/02-strandbound/main.cpp`'s `kVertexShader`, add:

```glsl
layout(location = 3) in vec3 aTangent;
out vec3 vTangent;
// ...
vTangent = mat3(uModel) * aTangent;
```

### Step 2 — Strandbound fragment shader: TBN + perturbed normal + Blinn-Phong

In `kFragmentShader`:

Add the new uniforms + input:
```glsl
in vec3 vTangent;
uniform sampler2D uNormalMap;
uniform sampler2D uSpecularMap;
uniform float uSpecPower;
```

In `main()`, after `vec3 n = normalize(vNormal);` (or wherever the normal
is first taken), compute the TBN + perturbed normal:

```glsl
vec3 N = normalize(vNormal);
vec3 T = normalize(vTangent);
vec3 B = cross(N, T);
mat3 TBN = mat3(T, B, N);
vec3 tangentNormal = texture(uNormalMap, vUV * uUvScale).rgb * 2.0 - 1.0;
vec3 perturbedN = normalize(TBN * tangentNormal);

vec3 V = normalize(uCameraPos - vWorldPos);
float specMask = texture(uSpecularMap, vUV * uUvScale).r;
```

Then **replace** the existing sun + point-light lighting code. The sun
block becomes:

```glsl
// Sun (diffuse + specular + shadow + ambient):
vec3 L = -normalize(uLightDir);
vec3 H = normalize(L + V);
float sunDiff = max(dot(perturbedN, L), 0.0);
float sunSpec = pow(max(dot(perturbedN, H), 0.0), uSpecPower);
vec3 lighting = uLightColor * (sunDiff * shadowFactor() + uAmbient
              + sunSpec * specMask);
```

(Note: `lighting` is being declared here, replacing whatever line currently
declares it. Read the existing code first.)

The point-light loop becomes:

```glsl
for (int i = 0; i < uPointLightCount; ++i) {
    vec3 toLight = uPointLights[i].position - vWorldPos;
    float dist = length(toLight);
    if (dist < 0.0001 || dist >= uPointLights[i].range) continue;
    vec3 Lp = toLight / dist;
    float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
    float diffuse = max(dot(perturbedN, Lp), 0.0);
    vec3 Hp = normalize(Lp + V);
    float spec = pow(max(dot(perturbedN, Hp), 0.0), uSpecPower);
    lighting += uPointLights[i].color * uPointLights[i].intensity * falloff
              * (diffuse + spec * specMask);
}
```

Everything after the point-light loop (texel sample, emissive, fog mix,
reflection composition, FragColor write) stays unchanged.

### Step 3 — Spinning-cube shader updates

In `games/01-spinning-cube/main.cpp`:
- Vertex shader: add `layout(location = 3) in vec3 aTangent; out vec3 vTangent;`
  and `vTangent = mat3(uModel) * aTangent;`.
- Fragment shader: declare `in vec3 vTangent; uniform sampler2D uNormalMap;
  uniform sampler2D uSpecularMap; uniform float uSpecPower;` near the other
  uniforms. Body unchanged (cube stays unlit textured; the uniforms exist
  only so uploads find locations).

### Step 4 — Renderer uploads + texture bindings

In `engine/render/backends/opengl/OpenGLRenderer.cpp`'s lit-pass per-draw
loop, after the existing reflection-uniform upload block, add:

```cpp
// Normal + specular maps + spec power.
shader.setInt("uNormalMap", 4);
shader.setInt("uSpecularMap", 5);
shader.setFloat("uSpecPower", call.material.specPower);

const TextureHandle nmHandle = (call.material.normalMap != kInvalidHandle)
                                 ? call.material.normalMap
                                 : flatNormalTexture_;
const TextureHandle spHandle = (call.material.specularMap != kInvalidHandle)
                                 ? call.material.specularMap
                                 : noSpecularTexture_;
if (nmHandle != kInvalidHandle && nmHandle <= textures_.size()) {
    textures_[nmHandle - 1]->bind(4);
}
if (spHandle != kInvalidHandle && spHandle <= textures_.size()) {
    textures_[spHandle - 1]->bind(5);
}
```

After the lit-pass loop, in the existing texture-unit cleanup block (which
already cleans units 1, 2, 3), add:

```cpp
glActiveTexture(GL_TEXTURE4);
glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(GL_TEXTURE5);
glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(GL_TEXTURE0);
```

(Match the existing cleanup pattern.)

### Step 5 — Build + test

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 pass. **Strandbound visuals change**:
fragments now use `perturbedN` (which equals the geometric normal when
the fallback flat-normal texture is bound — so lighting is the same), but
the math path is different. Sanity-check there are no obvious regressions
in lighting.

### Step 6 — Commit

```powershell
git add engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp games/02-strandbound/main.cpp
git commit -m @'
Lit shader: TBN normal mapping + Blinn-Phong specular

Strandbound's fragment shader builds a TBN matrix from
vTangent + vNormal, samples the normal map at unit 4 (or the
flat-normal fallback), and perturbs the shading normal before
all lighting math. Both sun and point-light contributions add
a Blinn-Phong specular term modulated by the spec map (unit 5,
black fallback). uSpecPower controls glossiness; default 32.

Spinning-cube shader declares the new uniforms (unused — cube
stays unlit textured). Renderer binds normal + spec textures
per draw with fallback to the new flat-normal/no-spec textures
when materials don't set them; cleans up units 4 and 5 after
the lit pass.

Default Material yields the geometric normal (perturbedN == N
via flat-normal fallback) and zero spec contribution (black
fallback), so behaviour is unchanged for any draw that doesn't
opt in.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 4: Procedural texture generators + Strandbound application

**Files:**
- Create: `engine/render/ProceduralTextures.h` (header-only generators)
- Modify: `games/02-strandbound/main.cpp` (generate + apply maps)

### Step 1 — Create the generator header

Create `engine/render/ProceduralTextures.h`:

```cpp
#pragma once

#include <cmath>
#include <vector>

namespace iron {

// Generates an RGBA8 normal map representing wooden plank seams along one
// axis. The output is `size * size * 4` bytes RGBA. Plank-seam grooves
// run vertically (along V); the perturbed normal tilts away from the
// seam, creating the visual impression of a recessed groove between
// planks. `planks` is the number of plank columns across the texture.
inline std::vector<unsigned char> generateWoodNormalMap(int size, int planks) {
    std::vector<unsigned char> out(static_cast<std::size_t>(size) * size * 4);
    const float invSize = 1.0f / static_cast<float>(size);
    const float twoPiPlanks = 2.0f * 3.14159265f * static_cast<float>(planks);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) * invSize;
            // dN/dU: derivative of sin(2*pi*planks*u) wrt u, sampled to
            // produce the X component of the tangent-space normal.
            // Sharp groove between planks: sin(...) crosses zero at each
            // seam; cos gives the slope, which is what we want for normals.
            const float slope = std::cos(twoPiPlanks * u);
            // Tangent-space normal: (slope*strength, 0, sqrt(1 - that^2)).
            // Strength = 0.3 keeps grooves visible without overdoing it.
            const float strength = 0.3f;
            float nx = slope * strength;
            const float nz = std::sqrt(1.0f - nx * nx);
            const float ny = 0.0f;
            // Pack into [0,255] RGB; A = 255.
            const int idx = (y * size + x) * 4;
            out[idx + 0] = static_cast<unsigned char>((nx * 0.5f + 0.5f) * 255.0f);
            out[idx + 1] = static_cast<unsigned char>((ny * 0.5f + 0.5f) * 255.0f);
            out[idx + 2] = static_cast<unsigned char>((nz * 0.5f + 0.5f) * 255.0f);
            out[idx + 3] = 255;
        }
    }
    return out;
}

// Generates an RGBA8 specular mask: uniform high spec everywhere.
// Output is `size * size * 4` bytes RGBA. Greyscale (R=G=B); the lit
// shader samples the R channel.
inline std::vector<unsigned char> generateMetalSpecularMap(int size) {
    std::vector<unsigned char> out(static_cast<std::size_t>(size) * size * 4,
                                    200);
    // Re-pack so RGB = 200 and A = 255.
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i + 0] = 200;
        out[i + 1] = 200;
        out[i + 2] = 200;
        out[i + 3] = 255;
    }
    return out;
}

} // namespace iron
```

### Step 2 — Generate textures in Strandbound

In `games/02-strandbound/main.cpp`, near the existing
`renderer.loadTexture(... crate.jpg)` line (or at the bottom of the
texture-creation block), add:

```cpp
#include "render/ProceduralTextures.h"  // near other engine includes

// ...

const auto woodNormalPixels = iron::generateWoodNormalMap(256, 8);
const iron::TextureHandle woodNormalTex =
    renderer.createTexture(256, 256, woodNormalPixels.data());

const auto metalSpecPixels = iron::generateMetalSpecularMap(256);
const iron::TextureHandle metalSpecTex =
    renderer.createTexture(256, 256, metalSpecPixels.data());
```

### Step 3 — Apply maps to Strandbound surfaces

In the `boxes[]` loop (the one that calls `makeBox(def, renderer, texture)`),
the resulting `RenderObject` only carries `texture` — the material fields
are set later during draw-call construction. We need to differentiate the
pole from the islands.

The current `boxes[]` array indexes are:
- `[0]` home island
- `[1]`, `[2]`, `[3]` props
- `[4]` far island
- `[5]` pole

Find the draw-call construction in the render lambda (where scene.objects
are submitted as DrawCalls). For each object, set `dc.material.normalMap = woodNormalTex`.
For the pole (index 5 in scene.objects), additionally set
`dc.material.specularMap = metalSpecTex` and `dc.material.specPower = 64.0f`.

If the render lambda doesn't know object indices easily, an inline lambda
or a per-BoxDef tag will work. Read the existing code and choose the
cleanest split.

**Bulbs, water, ropes:** leave at defaults (no normal map, no spec map).

### Step 4 — Build + test + playtest

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 14/14 pass. Strandbound visuals: islands and props
should show visible plank-seam grooves; the pole should show a bright
highlight that tracks the sun direction.

### Step 5 — Commit

```powershell
git add engine/render/ProceduralTextures.h games/02-strandbound/main.cpp
git commit -m @'
Procedural wood-normal and metal-spec maps; apply to Strandbound

Adds engine/render/ProceduralTextures.h with two generators:
generateWoodNormalMap (sine-wave plank grooves) and
generateMetalSpecularMap (uniform high-spec greyscale).
Strandbound builds both at startup and applies the wood normal
to islands and props; the pole gets the wood normal + the metal
spec + specPower=64 for a polished-wood highlight.

Bulbs, water, and ropes stay at defaults (no normal/spec maps).

CC0 PNG textures (real wood + metal) are the next milestone
alongside the showcase scene.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 5: Docs

**File:** `docs/engine/lighting.md`

Append a `## Normal maps and specular` section after the existing
`## Materials` section.

### Step 1 — Insert section

```markdown
## Normal maps and specular

**Normal maps** perturb the shading normal per fragment so flat geometry
looks bumpy. Each mesh vertex carries a `tangent` direction (the U axis
of its UV layout); the fragment shader builds a TBN matrix from
`(tangent, cross(normal, tangent), normal)`, samples the normal map (a
texture whose RGB encodes a vector in tangent space, decoded as
`rgb * 2 - 1`), and transforms the sampled normal into world space. All
the lighting math (sun, point lights, shadows) uses this perturbed
normal. Default `Material{}` has `normalMap = kInvalidHandle`, and the
renderer binds a 1×1 flat-normal fallback (`RGB 128,128,255` = +Z in
tangent space), so undecorated surfaces look identical to before.

**Specular highlights** are Blinn-Phong: for each light, the half-vector
`H = normalize(L + V)` between the light direction and view direction is
dotted with the (perturbed) normal, raised to `uSpecPower`, and added to
the lighting on top of the diffuse term. `Material::specularMap` (a
greyscale texture sampled on R) masks the highlight per fragment so only
"glossy patches" reflect brightly; `Material::specPower` (default 32)
controls the tightness — higher means smaller, sharper highlights.

Together: a wood plank with a normal map and no spec map shows grooved
diffuse shading; a metal pole with both shows grooves *and* a tight
highlight that tracks the sun and lanterns.

Future milestones may add PBR (roughness/metalness), parallax mapping,
or per-light specular tuning. For now: Blinn-Phong, one exponent per
draw, one greyscale mask.
```

### Step 2 — Commit

```powershell
git add docs/engine/lighting.md
git commit -m @'
Document normal maps and Blinn-Phong specular

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Self-review (planner)

- [ ] Spec coverage: tangent (Task 1) + Material fields (Task 2) + fallback
  textures (Task 2) + lit shader (Task 3) + uniform uploads + texture
  binds + cleanup (Task 3) + cube uniform decls (Task 3) + procedural
  generators (Task 4) + Strandbound integration (Task 4) + docs (Task 5).
  All covered.
- [ ] No placeholders. ✓
- [ ] Type consistency: `Vertex::tangent` (Task 1) — `aTangent`
  (location 3) in shaders (Task 3). `Material::normalMap`/`specularMap`/
  `specPower` (Task 2) — `uNormalMap` (unit 4) / `uSpecularMap` (unit 5)
  / `uSpecPower` (Task 3). `flatNormalTexture()` / `noSpecularTexture()`
  (Task 2) — used as fallback in lit-pass loop (Task 3). ✓
- [ ] Atomicity: Task 1 adds tangent attribute but shaders don't read it
  yet — fine because GL silently ignores unread attributes. Task 3 wires
  the shader-side; by then attribute 3 is registered.
