# Shadow Mapping — Design

**Status:** Approved 2026-05-22.

## Goal

Give the engine real-time **shadows**. The directional light (the sun) casts
shadows: the floating island, the props, the pole, and the ropes drop shadows
onto the surfaces below them. This is the engine's first multi-pass render
feature and its first use of render-to-texture.

## Context

The Iron Core Engine renders a lit scene through an API-agnostic RHI with an
OpenGL 3.3 backend. Today the renderer is **immediate-mode**: `submit` draws
each object the instant it is called, lit by a single Lambert directional
light with flat ambient. There is no render-to-texture, no shadowing — every
surface is lit purely by its normal's angle to the sun, so the scene reads as
flat.

The engine has: a hand-written math library (`Vec`, `Mat4`, `Transform` with
`perspective` and `lookAt` but **no** `orthographic`), the RHI
(`createMesh`/`updateMesh`/`createTexture`/`loadTexture`/`createShader`/
`submit`/`beginFrame`/`endFrame`/`drawLine`/`flushDebugLines`/`drawHud`), the
OpenGL backend (`GLMesh`, `GLShader`, `GLTexture`, `GLDebugLines`, `GLHud`,
`OpenGLRenderer`), a `DirectionalLight`, a minimal `Scene`, and the game
`games/02-strandbound`, which authors its own lit-shader GLSL and submits a
list of `RenderObject`s plus its rope geometry each frame.

Shadow mapping is the first of a planned set of lighting/visual milestones
(point lights, reflections, materials follow). It is chosen first because it
is the highest-impact upgrade, and the render-to-texture machinery it
introduces is later reused by reflections.

## Scope

### In scope

- **A buffered, multi-pass renderer.** `submit` records draw calls; the
  renderer replays them in two passes per frame.
- **Render-to-texture:** a depth-only framebuffer (`GLShadowMap`).
- **The shadow pass:** the scene's depth rendered from the sun's viewpoint.
- **The lit pass:** the normal lit render, now sampling the shadow map so
  surfaces in shadow are darkened.
- **`orthographic()`** added to the math library (the directional light's
  projection), pure and unit-tested.
- Hard shadows (a single depth comparison per fragment) with a depth bias.
- The Strandbound lit shader updated to be shadow-aware.

### Out of scope (deliberate)

- **Soft shadows (PCF).** Hard shadows only this milestone; PCF is a small
  follow-up.
- **Point-light or spotlight shadows.** Only the one directional light casts
  shadows. (There are no point lights yet.)
- **Cascaded shadow maps.** A single shadow map with a fixed scene-covering
  frustum — no cascades.
- **Reflections, fog, sky, materials.** Separate future milestones. (The
  render-to-texture machinery added here is what reflections will later build
  on.)
- **A new demo scene.** Shadows are shown in the existing Strandbound level.

## Design

### Architecture: the buffered, multi-pass renderer

Shadow mapping needs two passes per frame — depth-from-the-light, then the lit
render — so the renderer can no longer draw objects the instant they are
submitted. The renderer becomes **buffered**: it records the frame's draw
calls and replays them.

- **`beginFrame`** gains the camera. Its new signature is
  `beginFrame(Vec3 clearColor, const DirectionalLight& light, const Mat4& view,
  const Mat4& projection)`. It clears the recorded draw-call list and stores
  the camera and light for the frame.
- **`submit`** loses the camera. Its new signature is `submit(const DrawCall&
  call)`. It **records** the call (mesh, shader, texture, model) into a
  per-frame list — it does not draw.
- **`endFrame`** executes the frame in two passes (below).
- **`drawLine` / `flushDebugLines` / `drawHud`** are unchanged. They are
  overlays: the game calls `flushDebugLines` and `drawHud` *after* `endFrame`,
  so they draw on top of the finished lit scene. (`flushDebugLines` keeps its
  `view` / `projection` parameters.)

This is a one-time renderer change that every future multi-pass feature
(reflections, post-processing) builds on.

### 1. The shadow map — render-to-texture

A new backend object `GLShadowMap` owns a framebuffer object with a single
**depth texture** attachment (no colour attachment) at a fixed resolution
(`2048 × 2048`). It exposes: binding the FBO as the render target, binding the
depth texture to a texture unit for sampling, and the resolution. The depth
texture uses clamp-to-edge with a border so samples outside the map read as
"lit."

`OpenGLRenderer` owns exactly one `GLShadowMap` internally — it is an engine
implementation detail and never appears in the RHI surface.

### 2. The light-space matrix — `orthographic()`

A directional light has a direction but no position, so its "camera" is an
**orthographic** projection aimed along the light direction.

`engine/math/Transform.h` gains a pure `orthographic(float left, float right,
float bottom, float top, float nearZ, float farZ)` builder — a column-major
`Mat4` mapping the box to OpenGL clip space, matching the right-handed,
-Z-forward convention of the existing `lookAt` / `perspective`.

The renderer builds the **light view-projection** each frame:
`lightViewProj = orthographic(...) · lookAt(eye, eye + lightDir, up)`, where
`eye` is placed up-light from the scene centre and the orthographic box is
sized to enclose the scene. The renderer holds a settable **shadow scene
bounds** (a centre and a radius) used to place `eye` and size the box; it
defaults to values that cover the whole Strandbound level (the two islands
and the gap). The bias direction (`up`) is chosen perpendicular to the light
direction.

### 3. The shadow pass

In `endFrame`, pass one:

1. Bind the `GLShadowMap`'s FBO; set the viewport to the shadow-map
   resolution; clear its depth.
2. Bind a minimal **depth-only shader** — an engine-owned `GLShader` created
   once by `OpenGLRenderer` (a vertex stage of `uLightViewProj · uModel ·
   position`; an empty fragment stage — OpenGL writes depth automatically).
3. For every recorded draw call, set `uModel` and draw the mesh. Texture and
   the call's own shader are ignored — only depth matters.
4. Unbind the FBO; restore the viewport to the window size.

Front-face culling during this pass (or a depth bias) reduces shadow acne; a
depth bias in the comparison (step 4) is the primary defence and is the chosen
approach here.

### 4. The lit pass

In `endFrame`, pass two:

1. Bind the default framebuffer; clear colour and depth.
2. For every recorded draw call: bind the call's shader, set
   `uModel` / `uView` / `uProjection` and the directional-light uniforms (as
   today), **and additionally**: bind the shadow map's depth texture to
   **texture unit 1**, set `uShadowMap` to 1, set `uLightViewProj`, and set
   `uShadowBias`. Bind the call's colour texture to unit 0. Draw the mesh.

The convention — unit 0 is the colour texture, unit 1 is the shadow map,
`uLightViewProj` / `uShadowBias` are set by the renderer — mirrors the
existing convention for `uTexture` / `uLightDir` / `uLightColor` / `uAmbient`.

### 5. The lit shader's shadow term

The Strandbound lit shader (authored in `games/02-strandbound/main.cpp`) is
updated:

- The **vertex shader** outputs the fragment's light-space position:
  `vLightSpacePos = uLightViewProj · uModel · vec4(aPos, 1.0)`.
- The **fragment shader** computes a shadow factor:
  - Map `vLightSpacePos` to `[0, 1]` shadow-map coordinates.
  - If the coordinate is outside the map, the fragment is lit (factor 1).
  - Otherwise sample the shadow map's stored depth and compare it to the
    fragment's light-space depth minus `uShadowBias`. Farther than stored →
    in shadow (factor 0); otherwise lit (factor 1).
  - The shadow factor multiplies **only the diffuse** contribution; the
    ambient term is unaffected, so a shadowed surface is dim, not black.

The result: `lighting = uLightColor · (diffuse · shadow + uAmbient)`.

Because Strandbound's rope tubes are submitted through the same lit path, they
cast and receive shadows along with everything else.

### File layout

```
engine/math/Transform.h                               orthographic() builder        (modified)
engine/render/backends/opengl/GLShadowMap.h, .cpp      depth FBO + depth texture     (new)
engine/render/Renderer.h                               beginFrame/submit signatures  (modified)
engine/render/backends/opengl/OpenGLRenderer.h, .cpp   buffered two-pass + shadows   (modified)
engine/CMakeLists.txt                                  register GLShadowMap.cpp      (modified)
games/02-strandbound/main.cpp        new beginFrame/submit calls; shadow-aware lit shader (modified)
games/02-strandbound/RopeTool.h, .cpp                  new submit signature          (modified)
tests/test_transform.cpp                               orthographic() test           (modified)
docs/engine/shadow-mapping.md                          concept note                  (new)
```

## Testing

- **`orthographic()`** is pure math — unit-tested in the existing
  `tests/test_transform.cpp`: the box's `left`/`right`/`bottom`/`top`/near/far
  planes map to the corresponding edges of OpenGL clip space (`-1`/`+1`); a
  point at the box centre maps to the clip-space origin.
- The **render-to-texture, the shadow pass, the depth comparison, and the
  buffered two-pass replay** are GL code that the CTest harness cannot exercise
  (no GL context). They are verified by building and running Strandbound: the
  island, props, pole, and ropes cast visible shadows that move correctly with
  the sun direction, with no obvious shadow acne or peter-panning.
- Every existing unit test must still build and pass against the changed RHI
  signatures.

## Acceptance criteria

Launch `games/02-strandbound`. The directional light casts shadows: the props
and the pole drop shadows onto the island, the islands' edges shadow the gap,
and tied/thrown ropes cast shadows. Shadowed surfaces are dimmed (ambient
still lifts them — they are not pure black). Shadows are crisp (hard-edged).
Placing, throwing, cutting, mounting, tightrope-walking, and the win all still
work; the HUD and debug lines still draw on top. `Escape` quits. All unit
tests pass.

## Conventions

Unchanged from M1–M6: namespace `iron` for engine code; engine headers
included relative to `engine/`; `Mat4` column-major; C++23; CMake; commit
after every task with the `Co-Authored-By` trailer; MSVC multi-config tests
run with `ctest --test-dir build -C Debug --output-on-failure`. Work proceeds
on a feature branch; `main` is protected (PR + green CI required to merge).
