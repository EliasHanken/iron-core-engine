# Reflections — Design

**Date:** 2026-05-23
**Status:** Approved, pending implementation plan
**Milestone:** Third milestone on the visual/engine upgrades track (after
multiple point lights and atmosphere). Adds both cubemap environment
reflections and planar (mirrored-camera + RTT) reflections in one milestone,
sharing a single per-`DrawCall` reflectivity API.

## Goal

Add reflection rendering to the engine. Two complementary mechanisms share
one material API:

- **Cubemap environment reflections** — any reflective surface samples the
  active sky cubemap based on the reflected view direction. Cheap, applies
  to all surfaces, reflects the sunset for free.
- **Planar reflections** — a single world-space reflection plane drives an
  extra render pass each frame (mirrored camera + clip plane → off-screen
  RTT). Reflective surfaces above the plane sample the RTT in screen
  space, producing real reflections of dynamic geometry.

Light up Strandbound with a water plane (y = -3) that uses the planar
reflection, plus a subtle cubemap glaze on the existing boxes and ropes so
the sunset shows up on every surface.

## Non-goals

- **Screen-space reflections (SSR).** No depth-buffer ray-marching. Planar
  reflections cover the dynamic-reflection use case for this milestone.
- **Multiple reflection planes.** One plane per frame. Adding more would
  multiply the per-plane render cost.
- **Fresnel / view-angle-dependent reflectivity.** Reflectivity is a flat
  per-`DrawCall` float. Fresnel is a clean follow-up once a material
  system exists.
- **Roughness / blurred reflections.** Mirror-sharp only. Glossy /
  roughness would mean mipmapping the cubemap and/or convolving the planar
  RTT — deferred.
- **Reflective shadows / recursive reflections.** The reflection pass does
  not include shadows, point lights, fog, emissive, or other reflections.
  It is a deliberately simpler lit pass.
- **PBR.** Reflectivity is a single multiplier; not energy-conserving.
- **Refraction.** Water is a flat reflective surface, not refractive.

## High-level architecture

Two material additions on `DrawCall` (a float and a bool), one new
renderer verb (`setReflectionPlane`), one new render pass (planar
reflection between shadow and lit), and one new shader (the simplified
reflection-pass shader).

Per-frame data flow:

```
beginFrame(…)         → records sun + point lights + fog + view + projection
                        + the camera position (extracted from view⁻¹)

submit(DrawCall)      → buffers (existing behaviour)

endFrame() runs:
  1. Shadow depth pass — UNCHANGED.
  2. Planar reflection pass (NEW):
       only if a reflection plane is set;
       compute mirrored view from view * reflectionMatrix(plane);
       for each DrawCall with useReflectionPlane=false, render using the
       simplified reflection shader with gl_ClipDistance[0] discarding
       fragments below the plane;
       result goes into reflectionTarget_ (FBO with colour + depth RTT).
  3. Lit pass — gains reflection composition step at the end:
       reflective fragments mix in the cubemap (default) or the planar
       RTT (when useReflectionPlane=true), gated by reflectivity.
  4. Skybox pass — UNCHANGED.
```

## New types

```cpp
// engine/render/ReflectionPlane.h
struct ReflectionPlane {
    Vec3 normal{0.0f, 1.0f, 0.0f}; // unit-length world-space normal
    float d = 0.0f;                 // signed distance to origin along normal
};

// Mirror matrix for the plane. A point on the plane maps to itself; a
// point above maps to its reflection below. The matrix is its own inverse.
Mat4 reflectionMatrix(const ReflectionPlane& plane);
```

```cpp
// engine/render/Renderer.h — DrawCall additions:
struct DrawCall {
    // ... existing fields ...
    float reflectivity = 0.0f;        // 0 = matte, 1 = mirror
    bool useReflectionPlane = false;  // true: sample planar RTT; false: cubemap
};
```

## Renderer surface changes

```cpp
// engine/render/Renderer.h:

// Sets the world-space reflection plane. The renderer runs an extra
// reflection-render pass each frame using a camera mirrored across this
// plane; any DrawCall with useReflectionPlane=true samples the resulting
// texture in screen space. `normal` must be unit length.
virtual void setReflectionPlane(Vec3 normal, float d) = 0;

// Disables the planar reflection pass. DrawCalls with useReflectionPlane=
// true will fall back to sampling the cubemap (their reflection appears
// as the sky, not real geometry).
virtual void disableReflectionPlane() = 0;
```

`beginFrame` signature is unchanged. The fog parameter from the previous
milestone is the last virtual-interface addition for now.

## Shader changes

### Lit fragment shader (Strandbound's `kFragmentShader`)

New uniforms alongside the existing ones:

```glsl
uniform samplerCube uSkyCubemap;      // same cubemap as the skybox pass
uniform sampler2D uReflectionTexture; // planar reflection RTT
uniform float uReflectivity;          // per-draw
uniform int uUseReflectionPlane;      // per-draw, 1 = planar, 0 = cubemap
uniform vec2 uScreenSize;             // for screen-space UV
uniform vec3 uCameraPos;              // for reflected view direction
```

At the end of `main`, after the fog `mix`:

```glsl
// (existing code lands `vec3 finalColor` already fog-blended)

if (uReflectivity > 0.0) {
    vec3 reflectColor;
    if (uUseReflectionPlane == 1) {
        vec2 reflectUV = gl_FragCoord.xy / uScreenSize;
        reflectColor = texture(uReflectionTexture, reflectUV).rgb;
    } else {
        vec3 viewDir = normalize(vWorldPos - uCameraPos);
        vec3 reflectDir = reflect(viewDir, normalize(vNormal));
        reflectColor = texture(uSkyCubemap, reflectDir).rgb;
    }
    finalColor = mix(finalColor, reflectColor, uReflectivity);
}

FragColor = vec4(finalColor, texel.a);
```

Order matters: fog is "distance through atmosphere" and should affect the
lit colour BEFORE reflection. The reflection is "this surface has a shiny
finish" — added on top of the fogged lit colour.

### Reflection-pass shaders (new)

A simplified vertex/fragment shader pair, used ONLY for pass 2. No
shadows, no point lights, no fog, no emissive, no reflection — sun +
ambient + diffuse texture only.

```glsl
// Vertex:
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uReflectionView;
uniform mat4 uProjection;
uniform vec4 uClipPlane; // (normal.xyz, -d) — fragments where dot(pos,n)+w<0 are clipped
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_ClipDistance[0] = dot(worldPos4.xyz, uClipPlane.xyz) + uClipPlane.w;
    gl_Position = uProjection * uReflectionView * worldPos4;
}

// Fragment:
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;
void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    vec3 lighting = uLightColor * (diffuse + uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
}
```

C++ enables `glEnable(GL_CLIP_DISTANCE0)` for the reflection pass and
disables it after.

### Spinning-cube fragment shader

Declares the new uniforms (`uSkyCubemap`, `uReflectionTexture`,
`uReflectivity`, `uUseReflectionPlane`, `uScreenSize`, `uCameraPos`) so
the renderer's uploads don't silently no-op. Cube body unchanged. With
`uReflectivity = 0` default, the reflection block is never entered.

## Computing the mirrored view

Given a reflection plane with unit normal `N` and signed distance `d`:

```
P = N N^T   (3x3 outer product)
M3 = I - 2 * P              (reflects directions)
t  = 2 * d * N              (translation to handle d != 0)

reflectionMatrix(plane) =
  [ M3   t ]
  [ 0    1 ]
```

This matrix is its own inverse and reflects any point across the plane.
The reflection-pass view matrix is `view * reflectionMatrix(plane)`.

## Components and files

### New files

- `engine/render/ReflectionPlane.h` — `struct ReflectionPlane` and free
  function `Mat4 reflectionMatrix(const ReflectionPlane&)`.
- `engine/render/backends/opengl/GLReflectionTarget.h/.cpp` — FBO with
  colour RTT + depth RTT. Mirrors `GLShadowMap` pattern. `bindForWriting()`,
  `bindColorTexture(int unit)`, `isValid()`, `resolution()`.
- `tests/test_reflection.cpp` — unit tests for `reflectionMatrix` and the
  `DrawCall` reflection-field defaults.

### Modified files (engine)

- `engine/render/Renderer.h`:
  - `DrawCall::reflectivity` (float, default 0).
  - `DrawCall::useReflectionPlane` (bool, default false).
  - Virtuals: `setReflectionPlane`, `disableReflectionPlane`.
  - `#include "render/ReflectionPlane.h"`.
- `engine/render/backends/opengl/OpenGLRenderer.h/.cpp`:
  - Members: `std::optional<ReflectionPlane> reflectionPlane_`,
    `GLReflectionTarget reflectionTarget_`, `GLShader reflectionShader_`,
    `Vec3 cameraPos_`.
  - `setReflectionPlane` / `disableReflectionPlane` implementations.
  - In `beginFrame`, compute `cameraPos_` from `inverse(view_) * Vec4{0,0,0,1}`.
  - In `endFrame`, run the planar reflection pass between shadow and lit
    when `reflectionPlane_` has a value. Skip any DrawCall with
    `useReflectionPlane=true` (avoid water reflecting itself).
  - Lit pass uniform uploads:
    - Once per shader bind: `uSkyCubemap` (texture unit 2),
      `uReflectionTexture` (unit 3), `uScreenSize`, `uCameraPos`.
    - Per draw: `uReflectivity`, `uUseReflectionPlane`.
- `engine/math/Mat4.h`: add `Mat4 inverse(const Mat4&)` if it isn't already
  present (needed for camera-position extraction).
- `engine/scene/Mesh.h/.cpp`: add `void appendQuad(MeshData& out,
  Vec3 center, Vec2 size, Vec3 normal)`. Four vertices CCW around the
  normal, UVs `0..1`, two triangles.

### Modified files (games)

- `games/02-strandbound/main.cpp`:
  - Fragment shader: add the new uniforms and the reflection composition
    at the end of `main`.
  - At startup: `renderer.setReflectionPlane(Vec3{0,1,0}, -3.0f)`.
  - Build water mesh via `appendQuad(out, {0,0,0}, {60,60}, {0,1,0})`,
    `createMesh`.
  - Per frame: submit a water DrawCall with `reflectivity=0.85`,
    `useReflectionPlane=true`, `model = translation({0,-3,0})`,
    `texture = whiteTexture()`.
  - Per frame: existing boxes and ropes get `reflectivity = 0.08f`,
    `useReflectionPlane = false` (subtle cubemap glaze).
  - Lantern bulbs stay `reflectivity = 0` (light sources, not surfaces).
- `games/01-spinning-cube/main.cpp`:
  - Fragment shader: declare the new uniforms (unused). Body unchanged.
  - Do NOT call `setReflectionPlane`. Visually identical.

### New tests (`tests/test_reflection.cpp`)

Six small tests against the pure math + struct defaults:

1. Reflection across y = 0: `(1, 2, 3)` → `(1, -2, 3)`.
2. Reflection across y = -3 (plane normal {0,1,0}, d = -3): `(0, 1, 0)` → `(0, -7, 0)`.
3. `M * M ≈ I` (the matrix is its own inverse).
4. A point on the plane reflects to itself.
5. A point above the plane and its reflection are equidistant from the plane.
6. Default `DrawCall::reflectivity == 0` and `useReflectionPlane == false`.

### CMake

- `engine/CMakeLists.txt`: add `GLReflectionTarget.cpp`.
- `tests/CMakeLists.txt`: register `test_reflection`.

### Not touched

- Shadow mapping pipeline, `GLShadowMap`, depth shader.
- Point-light upload, fog uniforms, skybox pass.
- HUD subsystem, debug lines, rope physics, controller, raycasting.

## Constants

```cpp
// OpenGLRenderer.cpp internal:
constexpr int kReflectionResolution = 1024;
// Matches a reasonable framebuffer size for clarity without the
// expense of a full-resolution scene render. The reflection RTT is
// kReflectionResolution x kReflectionResolution.
```

## Error handling and edge cases

- **No reflection plane set:** the reflection pass is skipped entirely.
  Any DrawCall with `useReflectionPlane=true` falls back to sampling the
  cubemap (the planar texture is bound but unused because the
  `uUseReflectionPlane` uniform is 0 in that case — actually, since the
  game sets `useReflectionPlane=true` per-draw, the shader will sample
  the unbound texture and produce black. **Resolution:** the lit pass
  forces `uUseReflectionPlane = 0` when `reflectionPlane_` is unset,
  regardless of the per-DrawCall flag. This way the cubemap shows through
  for water-surface fragments when reflections are disabled — better than
  black artifacts.
- **`reflectivity = 0`:** the entire reflection block is skipped via the
  `if (uReflectivity > 0.0)` guard. Identical performance and visuals to
  the previous milestone.
- **Camera below the reflection plane:** the clip plane would discard all
  geometry. **Resolution:** acceptable — the player can see "incorrect"
  reflection (the cubemap) until they come back above. A future milestone
  could invert the clip plane direction based on camera position; not now.
- **Reflective surface itself appearing in the reflection (recursion):**
  DrawCalls with `useReflectionPlane=true` are skipped in the reflection
  pass. The water doesn't reflect itself.
- **Reflection RTT initialisation failure:** `GLReflectionTarget::isValid()`
  returns false; the renderer logs a warning and skips the reflection
  pass; reflective DrawCalls fall back to cubemap.

## Testing

### Unit tests

Six tests in `tests/test_reflection.cpp` per the components section. Suite
goes from 13 → 14 test binaries.

### Manual playtest checklist

- [ ] Walk to the edge of the home island and look down — see the sunset
      sky, the home island, the bridge anchor pole, and the far island
      reflected in the water below.
- [ ] Walk along the edge — reflection tracks the camera correctly (turn
      head, reflection rotates with you).
- [ ] Boxes / ropes show a subtle warm sunset glow from the cubemap
      reflection.
- [ ] Looking at the water at a glancing angle along the surface: the
      reflection shows the horizon clearly.
- [ ] Sun shadows still cast on land surfaces (the water doesn't break
      them).
- [ ] Point lights still warm nearby ground (water is reflective, ground
      is not).
- [ ] Lantern bulbs do not become reflective (they stay emissive).
- [ ] `games/01-spinning-cube` is visually identical to before.
- [ ] No GL errors in the log on startup or per frame.
- [ ] Reflection RTT isn't black (would indicate the reflection pass
      isn't rendering).
- [ ] Water surface isn't pure black (would indicate `uReflectionTexture`
      isn't bound).

## Out of scope (carried forward to future milestones)

- Fresnel and view-angle reflectivity.
- Roughness / mipmapped or convolved reflection blur.
- Multiple reflection planes.
- Refraction (water bending the light from below).
- Screen-space reflections (SSR).
- Recursive reflections (reflections inside reflections).
- Reflective shadows / PBR.
- A separate water shader with normal-map ripples — the current water is a
  flat-mirror surface. Normal maps come in the materials milestone.

## References

- Previous milestone: atmosphere (`docs/superpowers/specs/2026-05-23-atmosphere-design.md`).
- The buffered renderer + render-to-texture pattern was introduced in the
  shadow-mapping milestone; the planar reflection pass reuses it.
- The cubemap from the atmosphere milestone is the source of environment
  reflection.
