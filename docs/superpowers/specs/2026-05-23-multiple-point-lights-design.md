# Multiple Point Lights — Design

**Date:** 2026-05-23
**Status:** Approved, pending implementation plan
**Milestone:** First milestone of the visual/engine upgrades track after shadow
mapping. Adds local omnidirectional lighting on top of the existing single
directional sun.

## Goal

Add support for up to 16 unshadowed **point lights** to the lit render path,
plus a per-`DrawCall` **emissive** colour so a light's source mesh (a lantern
bulb, a torch flame, a glowing crystal) actually appears bright. Light up
Strandbound with three point lights — a warm home lantern, a cool bridge
marker, a warm far-island goal — to playtest the feature in a scene we
already know.

## Non-goals

- **Point-light shadows.** No omnidirectional shadow maps in this milestone.
  The sun's shadow map keeps working as today; point lights light surfaces
  but don't darken occluded geometry. Cubemap shadow casting is a future
  focused milestone.
- **Spot lights.** Cone-shaped lights deferred. Adding them later is additive.
- **Multiple directional lights.** One sun is still the rule.
- **Specular term.** The lit shader stays Lambert + ambient as today. Adding
  specular is a separate small milestone if desired.
- **HDR / tonemapping.** Range-based smoothstep falloff is the deliberate
  choice precisely so we don't need HDR-range intensity values.
- **A new demo scene.** Lights go into the existing `games/02-strandbound`.
  A dedicated `games/03-...` showcase is its own future milestone.

## High-level architecture

A point light is just data — position, colour, intensity, world-space range.
The game owns a list of them on its `Scene` and hands the list to the
renderer once per frame, alongside the existing sun. The lit fragment
shader loops over the active point lights per fragment, applying Lambert and
a range-based smoothstep falloff. Visible source meshes (lantern bulbs) are
drawn with a non-zero `DrawCall::emissive`, which the fragment shader adds
on top of the lighting result so the bulb glows regardless of incoming light.

No shadow casting from point lights; the sun's shadow pipeline is untouched.

## New types

```cpp
// engine/render/Light.h — alongside the existing DirectionalLight.
struct PointLight {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};   // light colour
    float intensity = 1.0f;          // multiplier
    float range = 5.0f;              // world-space radius where contribution → 0
};
```

```cpp
// engine/render/Renderer.h — public cap, shared with shader array size.
constexpr int kMaxPointLights = 16;
```

```cpp
// engine/render/Renderer.h — DrawCall gains an optional emissive colour.
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Vec3 emissive{0.0f, 0.0f, 0.0f}; // added on top of lighting; default = no glow
};
```

```cpp
// engine/scene/Scene.h — Scene grows a list of point lights.
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
    std::vector<PointLight> pointLights; // NEW
};
```

## Renderer surface changes

`Renderer::beginFrame` grows one parameter — a span of point lights for this
frame, alongside the existing sun, view, and projection. The implementation
silently caps the list at `kMaxPointLights` (overflow is logged once per
frame in debug, never crashes):

```cpp
virtual void beginFrame(Vec3 clearColor,
                        const DirectionalLight& light,
                        std::span<const PointLight> pointLights,
                        const Mat4& view,
                        const Mat4& projection) = 0;
```

`submit(const DrawCall& call)` is unchanged in signature — the `emissive`
field is just a new optional field on the struct.

Point lights are **frame-state**, not draw-state: uploaded once per frame to
the lit shader's uniform array, not per draw call. This mirrors how the
directional light and view/projection are handled today.

## Per-frame data flow

1. Game updates per-light state (e.g. flicker via `sin(time)`).
2. Game calls
   `renderer.beginFrame(clearColor, scene.light, scene.pointLights, view, proj)`.
3. Renderer records the clear colour, sun, point-light list (capped to 16),
   view, and projection. It also runs the existing shadow-pass setup
   (`computeLightViewProj`).
4. Renderer's lit-pass shader bind uploads the point-light uniform array
   once per frame.
5. Each `submit(DrawCall)` records a draw, including its `emissive` colour.
6. `endFrame` runs the depth pass (sun only, unchanged), then the lit pass.
   In the lit pass, each draw call sets its `uEmissive` uniform and renders;
   the fragment shader loops over the per-frame point lights.

## Shader changes

The lit shader (the one used by `games/01-spinning-cube` and
`games/02-strandbound`) gains a uniform array of point lights and an
emissive uniform per draw. Vertex shader is unchanged — it already passes
`vWorldPos` and `vNormal` (added during shadow mapping).

```glsl
// New uniforms in the fragment shader:
struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
};
uniform PointLight uPointLights[16];
uniform int uPointLightCount;
uniform vec3 uEmissive;

// Fragment shader body — additions clearly marked.
vec3 normal = normalize(vNormal);

// Existing sun + shadow + ambient.
float sunLambert = max(dot(normal, -uLightDir), 0.0);
vec3 lighting = uLightColor * sunLambert * shadowFactor()
              + uLightColor * uAmbient;

// NEW — per-point-light contribution.
for (int i = 0; i < uPointLightCount; ++i) {
    vec3 toLight = uPointLights[i].position - vWorldPos;
    float dist = length(toLight);
    // Cull: outside range OR degenerate zero-distance (avoid NaN from
    // normalize(0)). The `< 0.0001` is much smaller than any realistic
    // fragment-to-light spacing, so this only fires at the singularity.
    if (dist < 0.0001 || dist >= uPointLights[i].range) continue;

    vec3 L = toLight / dist;
    float lambert = max(dot(normal, L), 0.0);
    float falloff = 1.0 - smoothstep(0.0, uPointLights[i].range, dist);
    lighting += uPointLights[i].color
              * uPointLights[i].intensity
              * lambert
              * falloff;
}

vec3 albedo = texture(uTex, vUv).rgb;
vec3 result = albedo * lighting + uEmissive; // NEW — emissive added on top
fragColor = vec4(result, 1.0);
```

Notes on the math:
- **Range-based smoothstep falloff** (`1 - smoothstep(0, range, dist)`) was
  chosen over physical inverse-square because it gives a single intuitive
  authoring parameter (the range), maps cleanly to a cheap cull, and does
  not need HDR intensity values to look right in LDR output. Big modern
  engines (Unity URP, Godot, Unreal mobile) use the same family.
- The `if (dist >= range) continue` is a free optimisation — outside the
  range, contribution would be zero anyway.

## Components and files

### New files

- `engine/render/Light.h` — add `PointLight` struct alongside
  `DirectionalLight` (same file, single-responsibility: "light data").
- `engine/render/PointLightMath.h` — pure header with one inline function
  `pointLightContribution(const PointLight&, Vec3 fragPos, Vec3 normal)`
  that mirrors the shader math so we can unit-test the falloff without
  spinning up a GL context.
- `tests/test_point_lights.cpp` — unit tests, see "Testing" below.

### Modified files (engine)

- `engine/render/Renderer.h` — `beginFrame` signature grows the
  `std::span<const PointLight>` parameter; `DrawCall` gains `emissive`;
  `constexpr int kMaxPointLights = 16` declared at namespace scope.
- `engine/render/backends/opengl/OpenGLRenderer.h/.cpp` — store the frame's
  point-light list (capped); upload `uPointLights[]` + `uPointLightCount`
  once per lit-pass shader bind; set `uEmissive` per draw call; emit a
  once-per-frame warning when overflow occurs.
- `engine/render/backends/opengl/GLShader.h/.cpp` — add
  `setInt(name, value)` and `setPointLight(name, const PointLight&)`
  helpers. The point-light helper looks up the 4 sub-uniforms
  (`<name>.position`, `<name>.color`, `<name>.intensity`, `<name>.range`)
  and sets them. Cache the location lookups exactly the way existing
  setters do.
- `engine/scene/Scene.h` — `std::vector<PointLight> pointLights` field.

### Modified files (games)

- `games/02-strandbound/main.cpp`:
  - Fragment shader gets the uniform array + emissive add per Section 2.
  - Build a small `std::vector<PointLight>` per frame:
    - Home lantern: position above home start, colour `(1.0, 0.7, 0.35)`,
      intensity 1.5, range 8.
    - Bridge marker: position above the bridge anchor pole, colour
      `(0.35, 0.6, 1.0)`, intensity 1.2, range 6.
    - Far-island goal: position above the win point, colour
      `(1.0, 0.85, 0.55)`, intensity 2.0, range 10.
  - Apply a per-light flicker (different `sin(time)` frequencies/phases so
    they do not pulse in sync).
  - Drop sun ambient from current value to 0.15 so the points have
    something to fight.
  - Add three small (~0.3-unit) "bulb" cube meshes at the same positions as
    the lights, drawn via `submit(DrawCall)` with `emissive` set to the
    matching light colour and `texture = whiteTexture()`.
  - Pass `scene.pointLights` into `beginFrame`.
- `games/01-spinning-cube/main.cpp`:
  - Fragment shader updated to the same uniform layout for consistency,
    but the game passes an empty span and `emissive = 0` everywhere, so
    visuals are unchanged.

### Not touched

- Shadow mapping pipeline (`GLShadowMap`, depth shader, light frustum).
- HUD subsystem.
- Rope physics, rope rendering, controller, raycasting, anything outside
  the lit pass.
- M2/M5/M6 game state machines.

## Error handling and edge cases

- **Overflow past 16 lights:** silently keep the first 16; in debug builds
  log a warning once per frame. No crash, no exception.
- **Light with `range <= 0`:** contributes zero (the smoothstep handles
  this; the cull catches the degenerate case).
- **Light with `intensity = 0` or `color = (0,0,0)`:** contributes zero;
  no special-case needed.
- **Light placed on top of a fragment (`dist ≈ 0`):** the shader's
  `dist < 0.0001` guard skips the light, contribution is zero. (In
  practice this is unreachable in the scene proper — but the bulb-mesh
  vertices sit *at* the light's position, so without the guard one
  fragment would go to NaN. The guard prevents it.)
- **Empty `pointLights` span:** `uPointLightCount = 0`, loop runs zero
  iterations, scene lit exactly as today.
- **`DrawCall::emissive` default `(0,0,0)`:** add-on-top is a no-op, so
  all existing draw calls keep their current appearance.

## Testing

### Unit tests (`tests/test_point_lights.cpp`)

Five small tests, all running against `pointLightContribution()` from the
shared header so the math is verified without a GL context:

1. **`PointLight` defaults.** Default-constructed light has position
   `(0,0,0)`, colour `(1,1,1)`, intensity 1, range 5.
2. **Distance = 0 (degenerate).** Contribution is exactly zero — the
   `dist < 0.0001` guard catches the singularity (`normalize(0)` would
   produce NaN). No NaN appears in the output.
3. **Distance ≥ range.** Contribution is exactly zero (the cull).
4. **Distance = range/2.** Contribution is strictly between 0 and the
   max — monotonic check.
5. **Normal facing away.** With `dot(normal, L) ≤ 0`, contribution is zero
   regardless of distance.

Sixth optional test:
6. **`DrawCall::emissive` default.** Default-constructed `DrawCall` has
   `emissive == Vec3{0, 0, 0}` — pins the contract.

Test count delta: +5 to +6, bringing the suite from its current ~80 to
~85-86.

### Manual playtest checklist

Recorded in this spec; run before the PR merges:

- [ ] Stand next to the home lantern — ground around it warms up; lantern
      bulb visibly glows.
- [ ] Bridge marker reads as a cool blue beacon from the home start.
- [ ] Far-island goal grows brighter as you approach.
- [ ] Sun-cast shadows still work (point lights don't fight them).
- [ ] `games/01-spinning-cube` is visually identical to before
      (zero point lights, zero emissive).
- [ ] No FPS regression on the dev machine.

## Out of scope (carried forward to future milestones)

- Spot lights (cone-shaped) — additive future milestone.
- Point-light shadows (cubemap depth) — its own focused milestone.
- Specular term — likely folded into a later "materials" milestone.
- HDR / tonemapping.
- Tile-based or clustered light culling.
- A dedicated lighting showcase scene (`games/03-...`).

## References

- Previous milestone: shadow mapping (`docs/superpowers/specs/2026-05-22-shadow-mapping-design.md`).
- Buffered renderer pattern was established during shadow mapping; point
  lights reuse it (frame-state uploaded once per frame).
