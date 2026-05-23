# Atmosphere — Design

**Date:** 2026-05-23
**Status:** Approved, pending implementation plan
**Milestone:** Second milestone on the visual/engine upgrades track (after
multiple point lights). Adds exponential distance fog, a cubemap skybox, and
horizon fog blending. The sun disc is part of the cubemap painting — no
separate sun pass.

## Goal

Add **atmosphere** to the engine: exponential distance fog tinted to a
configurable colour, a cubemap-textured skybox drawn at infinity, and a
horizon blend that dissolves the line where geometry meets sky into a
soft fog band. Light up Strandbound with a sunset palette so the scene
reads as dusk: warm orange-pink fog, deep blue sky overhead, sun baked
into the painted cubemap.

## Non-goals

- **Procedural skybox.** No shader-generated sky gradient. The skybox is
  always a cubemap; content is whatever PNGs the game ships.
- **Separate sun disc pass.** The sun lives in the cubemap painting. No
  `drawSun()` or fullscreen sun quad. Adding a procedural sun is a future
  milestone if ever wanted.
- **Volumetric fog / clouds.** Distance fog is a per-fragment blend; no
  light shafts, no 3D fog volumes.
- **Screen-space fog post-pass.** The fog is computed inline in the lit
  fragment shader (the simpler architecture). A fullscreen depth-reading
  pass is its own future milestone, useful only if we also add bloom or
  tonemapping.
- **Per-light scattering.** Point lights do not contribute fog scattering;
  fog is purely a distance-based colour blend.
- **Day/night cycle.** The sky is static for this milestone. A timed
  cubemap swap is trivial later but adds nothing to this design.

## High-level architecture

Three small new pieces:

1. **Fog parameters** — `Fog { Vec3 color; float density; }` lives on
   `Scene` and gets passed into `Renderer::beginFrame`, alongside the sun
   and point lights.
2. **Cubemap texture support** — `Renderer::loadCubemap(std::array<std::string, 6>)`
   returns a `CubemapHandle`. OpenGL backend wraps a `GL_TEXTURE_CUBE_MAP`.
3. **Skybox render pass** — `Renderer::setSkybox(CubemapHandle)` registers
   the active sky; the renderer runs a skybox pass between the lit pass
   and the HUD pass.

Each lit fragment blends its colour toward the fog colour by
`1 - exp(-density * distFromCamera)`. The skybox pass renders a unit cube
centred at the camera, samples the cubemap, AND blends the cubemap colour
toward the fog colour near the horizon — that's the trick that makes the
silhouette where geometry meets sky soft instead of sharp.

## New types

```cpp
// engine/render/Fog.h
struct Fog {
    Vec3 color{0.7f, 0.6f, 0.5f}; // warm-grey by default
    float density = 0.0f;          // 0 = no fog (default); existing demos
                                    // pass this and see no visual change.
};
```

```cpp
// engine/render/Handles.h
using CubemapHandle = std::uint32_t;
// `0` is the invalid handle; non-zero values are vector-index + 1
// (matches MeshHandle / TextureHandle convention).
```

## Renderer surface changes

```cpp
// engine/render/Renderer.h

// Loads 6 face PNGs into a cubemap. Face order matches OpenGL:
// +X, -X, +Y, -Y, +Z, -Z (right, left, top, bottom, front, back).
// Returns kInvalidHandle on failure (any face missing or undecodable).
virtual CubemapHandle loadCubemap(
    const std::array<std::string, 6>& facePaths) = 0;

// Registers the cubemap to use as the skybox for subsequent frames.
// Passing kInvalidHandle disables the skybox pass.
virtual void setSkybox(CubemapHandle sky) = 0;

// beginFrame gains the Fog parameter, after the point-light list:
virtual void beginFrame(Vec3 clearColor,
                        const DirectionalLight& light,
                        std::span<const PointLight> pointLights,
                        const Fog& fog,
                        const Mat4& view,
                        const Mat4& projection) = 0;
```

## Per-frame data flow

1. Game calls `renderer.setSkybox(skyHandle)` **once** at startup, after
   the cubemap is loaded.
2. Per frame, game calls
   `beginFrame(clearColor, sun, pointLights, fog, view, projection)`.
3. Renderer records `fog_`, `view_`, `projection_`, `skybox_`, etc.
4. Game submits `DrawCall`s as today.
5. `endFrame` runs four passes in order:
   - **Shadow depth pass** — unchanged.
   - **Lit pass** — for each draw call, the lit shader applies fog at
     the end via `mix(litColor, uFogColor, fogFactor)` where
     `fogFactor = 1 - exp(-uFogDensity * length(vViewPos))`.
   - **Skybox pass** — if `skybox_` is valid, renders a unit cube at the
     camera position with `GL_LEQUAL` depth and depth-write off, samples
     the cubemap, blends with `uFogColor` near the horizon.
   - **Debug-lines + HUD** — unchanged.

## Shader changes

### Lit shader (Strandbound's `kFragmentShader`)

Vertex shader gains an output:
```glsl
out vec3 vViewPos;
// ... in main():
vViewPos = (uView * uModel * vec4(aPos, 1.0)).xyz;
```

Fragment shader gains two uniforms and a fog blend at the end:
```glsl
in vec3 vViewPos;
uniform vec3 uFogColor;
uniform float uFogDensity;

// (existing point-light loop, emissive add, etc. unchanged)

void main() {
    // ... existing sun + shadow + point-light + emissive composition
    // landing as `vec3 litColor = texel.rgb * lighting + uEmissive;`

    float distFromCamera = length(vViewPos);
    float fogFactor = 1.0 - exp(-uFogDensity * distFromCamera);
    vec3 finalColor = mix(litColor, uFogColor, fogFactor);
    FragColor = vec4(finalColor, texel.a);
}
```

`length(vViewPos)` is the camera-to-fragment distance in world units —
the view matrix doesn't scale, so view-space length equals world-space
length.

### Skybox shader (new)

Vertex:
```glsl
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vWorldDir;
void main() {
    vWorldDir = aPos;
    // Strip translation from view so the sky never moves with the camera.
    mat4 viewNoTranslation = mat4(mat3(uView));
    vec4 clip = uProjection * viewNoTranslation * vec4(aPos, 1.0);
    // Force gl_FragDepth = 1 (far plane) so all geometry draws on top.
    gl_Position = clip.xyww;
}
```

Fragment:
```glsl
#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;
uniform samplerCube uSkyCubemap;
uniform vec3 uFogColor;
uniform float uHorizonFogBand;  // e.g. 0.25
void main() {
    vec3 dir = normalize(vWorldDir);
    vec3 skyColor = texture(uSkyCubemap, dir).rgb;
    // Blend with fog colour near the horizon. `abs(dir.y)` is 0 at
    // horizon, 1 at zenith/nadir. smoothstep gives a smooth onset.
    float horizonMix = smoothstep(0.0, uHorizonFogBand, abs(dir.y));
    vec3 result = mix(uFogColor, skyColor, horizonMix);
    FragColor = vec4(result, 1.0);
}
```

### Spinning-cube shader

Declares `uniform vec3 uFogColor; uniform float uFogDensity;` so the
renderer's per-frame uploads have targets; the cube body stays as it is
(`FragColor = texture(uTexture, vUV);`). Same pattern as the point-light
uniforms last milestone. `vViewPos` is NOT needed in the cube shader
because the fog math isn't applied. Visually identical.

## Components and files

### New files

- `engine/render/Fog.h` — the `Fog` struct.
- `engine/render/backends/opengl/GLCubemap.h/.cpp` — wraps
  `GL_TEXTURE_CUBE_MAP`. Loads 6 face PNGs via `stb_image`. `bind(int unit)`,
  `isValid()`.
- `engine/render/backends/opengl/GLSkybox.h/.cpp` — owns the 36-vertex
  unit cube VBO/VAO and the skybox `GLShader`. Exposes
  `draw(const Mat4& view, const Mat4& projection, const GLCubemap& sky,
   Vec3 fogColor, float horizonBand)`.

### Modified files (engine)

- `engine/render/Handles.h` — add `CubemapHandle` typedef.
- `engine/render/Renderer.h` — `loadCubemap`, `setSkybox`,
  `beginFrame`-with-`Fog` virtual signature. `#include "render/Fog.h"`.
- `engine/scene/Scene.h` — `Fog fog;` field on `Scene`.
- `engine/render/backends/opengl/OpenGLRenderer.h/.cpp`:
  - Store `Fog fog_` and `CubemapHandle skybox_` and a `std::vector<std::unique_ptr<GLCubemap>> cubemaps_`.
  - Implement `loadCubemap` (push back a GLCubemap, return index+1; `kInvalidHandle` on failure).
  - Implement `setSkybox` (record the handle).
  - In `endFrame`, after the lit pass, before debug lines + HUD: if
    `skybox_` is valid, call `skybox_.draw(view_, projection_,
    *cubemaps_[skybox_-1], fog_.color, kHorizonFogBand)`.
  - Upload `uFogColor` and `uFogDensity` per lit shader bind, same
    pattern as the sun uniforms.

### Modified files (games)

- `games/02-strandbound/main.cpp`:
  - Vertex shader: add `out vec3 vViewPos;` and compute it.
  - Fragment shader: add `uFogColor`, `uFogDensity` uniforms, `in vec3 vViewPos;`,
    and the fog `mix` at the end of `main`. Texture alpha (`texel.a`)
    preserved.
  - At startup: `CubemapHandle sky = renderer.loadCubemap({...6 paths...}); renderer.setSkybox(sky);`
  - Initialise `scene.fog = Fog{Vec3{0.8f, 0.55f, 0.4f}, 0.025f}` (warm
    sunset orange, density tuned for ≈50-unit visibility — adjust at
    implementation time to match the chosen cubemap's horizon).
  - Pass `scene.fog` into `beginFrame`.
- `games/01-spinning-cube/main.cpp`:
  - Fragment shader: declare `uFogColor`, `uFogDensity` uniforms (unused).
  - Pass `Fog{}` (default, density 0) to `beginFrame`. Visually identical.

### New assets

- `games/02-strandbound/assets/sky/` — six PNG faces from a CC0 sunset
  cubemap (Polyhaven or similar). Names: `px.png`, `nx.png`, `py.png`,
  `ny.png`, `pz.png`, `nz.png`. Total <2 MB.
- These are large binaries — verify Git LFS picks them up automatically
  (PNG is in the LFS attribute set already).

### Not touched

- Shadow mapping pipeline, the depth shader, `GLShadowMap`.
- Point-light upload code.
- HUD subsystem, debug lines, rope physics, controller, raycasting.
- Game state machines.

## Constants

```cpp
// OpenGLRenderer.cpp internal:
constexpr float kHorizonFogBand = 0.25f;
// Wider band = softer horizon dissolution. 0.25 = the horizon ramp
// spans ~14° of view angle.
```

## Error handling and edge cases

- **Cubemap load failure (any face missing or undecodable):**
  `loadCubemap` returns `kInvalidHandle`. The game can detect and skip
  `setSkybox`. The skybox pass is then a no-op; the clear colour shows
  through.
- **`setSkybox(kInvalidHandle)` or never called:** skybox pass is skipped;
  clear colour fills the framebuffer where geometry doesn't draw.
- **`fog.density = 0`:** `1 - exp(0) = 0`; fog blend is a no-op; existing
  demos look identical.
- **Camera inside a wall / very close to fragment:** `length(vViewPos)`
  is small, fog is small — no NaN, no artifacts.
- **Cubemap face mismatch (different sizes):** GL will error; we log and
  return `kInvalidHandle`. Caller can detect.

## Testing

No unit tests this milestone — fog/sky are pure GLSL with no CPU math
mirror worth maintaining. (Same pattern as the shadow-mapping milestone.)

**Manual playtest checklist** (run before PR merge):

- [ ] Sunset sky is visible all around when you look up and sideways.
- [ ] Camera rotation moves the sky; camera *translation* does not (skybox
      stays centred on the player).
- [ ] Horizon line between geometry and sky is soft — no sharp edge.
- [ ] Bridge anchor pole reads as visibly more fog-tinted than the home
      box.
- [ ] Far island looks distant — strongly fog-tinted, not crisp.
- [ ] Sun shadows still work (shadow map is rendered, lit pass samples
      it).
- [ ] Point lights still work (lanterns warm up nearby ground; bulbs
      glow).
- [ ] `games/01-spinning-cube` looks identical to before.
- [ ] No GL errors in the log on startup or per-frame.

## Out of scope (carried forward to future milestones)

- Procedural sun disc (separate fullscreen pass).
- Day/night cycle / animated skybox.
- HDR cubemaps, image-based lighting.
- Volumetric fog, light shafts.
- Per-point-light fog scattering (lanterns glowing through the fog).
- Screen-space fog post-pass (would be the right move once bloom /
  tonemap arrives).

## References

- Previous milestone: multiple point lights
  (`docs/superpowers/specs/2026-05-23-multiple-point-lights-design.md`).
- The buffered renderer pattern (introduced in the shadow-mapping
  milestone) is reused: fog is per-frame state stored in `beginFrame` and
  uploaded in the lit pass; the skybox pass slots in between lit and HUD.
- TF2-style cubemap aesthetic was the user's explicit reference for the
  sky look.
