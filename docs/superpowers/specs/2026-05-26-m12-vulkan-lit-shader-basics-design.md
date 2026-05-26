# M12 — Vulkan lit shader basics

**Status:** design approved 2026-05-26.

**Direction context:** Vulkan is the engine's forward path (see
`vulkan-only-direction` memory). M11 discovered that the M9 `submit`
path only uploads `mat4 mvp` per draw, so net-shooter ships unlit
textured on Vulkan. M12 fixes the lit pipeline foundation:
directional sun + ambient + per-draw emissive, propagated through a
richer per-draw UBO. Subsequent milestones add the more involved
lit-pass features (point lights, fog, shadow map, cubemap, normal
maps).

## Goals

1. Extend `VulkanRenderer::submit` to upload a richer per-draw UBO
   that carries sun direction/color, ambient, and per-draw emissive
   alongside the existing matrix data.
2. Update Vulkan shaders in `games/01-spinning-cube` and
   `games/07-net-shooter` to use the new UBO and render with
   Lambertian shading + emissive.
3. Verify both games still build and render under both backends.

After M12, the Vulkan-side net-shooter scene shows directional sun
shading (walls have light/dark gradient based on sun angle), rocket
tracer emissive cubes glow, and spinning-cube has visible facets as
it rotates.

## Scope

### In

- New `iron::LitUbo` struct (192 bytes) replacing the bare `Mat4`
  upload in `VulkanRenderer::submit`.
- New per-frame `pendingSunDir_` / `pendingSunColor_` /
  `pendingAmbient_` private state in `VulkanRenderer`. Populated by
  `beginFrame` from the `DirectionalLight` arg.
- Spinning-cube Vulkan shader rewrite (uses `LitUbo`, Lambertian +
  ambient + emissive).
- Net-shooter Vulkan shader rewrite (same approach). Update the
  one-time runtime warning to reflect what's now supported and what
  still isn't.
- Light-bound smoke verification on Vulkan for both games.

### Out (deferred to later milestones)

- Point lights (16-array).
- Fog (exponential distance).
- Shadow map sampling (needs the depth-pass port — separate
  milestone).
- Cubemap skybox + cubemap reflection sampling (needs cubemap
  pipeline — separate milestone).
- Planar reflection sampling (needs RTT pipeline — separate
  milestone).
- Normal maps + specular maps + TBN math (adds vertex tangent
  attribute usage in Vulkan + more sampler bindings — separate
  milestone, probably bundled with shadow port).
- Per-DrawCall UV scale (`call.material.uvScale`): the engine's
  existing GL shader supports it; we'll add when normal/spec maps
  come in.
- Polish (M11-era gizmo z-fight, HUD blur on Vulkan, wall texture
  pure-white on net-shooter): tracked as known issues, addressed
  later.

## Architecture

### `LitUbo` struct (engine-internal)

Defined privately inside `engine/render/backends/vulkan/VulkanRenderer.cpp`
(anonymous namespace) — not part of the public Renderer API.

```cpp
struct LitUbo {
    Mat4 mvp;        // 64 bytes — projection * view * model
    Mat4 model;      // 64 bytes — for normal transform via mat3(model)
    Vec4 sunDir;     // 16 bytes — xyz direction; w padding
    Vec4 sunColor;   // 16 bytes — xyz color; w padding
    Vec4 ambient;    // 16 bytes — xyz color; w padding
    Vec4 emissive;   // 16 bytes — xyz color; w padding (from call.material.emissive)
};
static_assert(sizeof(LitUbo) == 192, "LitUbo layout");
```

std140 compliant: all members are mat4 (64-byte aligned) or vec4
(16-byte aligned); no scalar straddling. Cumulative size is 192 bytes,
well within the 256-byte UBO alignment and the per-frame 256 KB UBO
sub-allocator budget.

### `VulkanRenderer` changes

In `VulkanRenderer.h`, add private state alongside
`pendingClear_` / `pendingView_` / `pendingProjection_`:

```cpp
Vec3 pendingSunDir_   = {0.0f, -1.0f, 0.0f};
Vec3 pendingSunColor_ = {1.0f, 1.0f, 1.0f};
Vec3 pendingAmbient_  = {0.1f, 0.1f, 0.1f};
```

In `VulkanRenderer::beginFrame`, after the existing
`pendingProjection_ = projection;` line, add:

```cpp
pendingSunDir_   = light.direction;
pendingSunColor_ = light.color;
pendingAmbient_  = Vec3{light.ambient * light.color.x,
                       light.ambient * light.color.y,
                       light.ambient * light.color.z};
```

The `ambient` field on `DirectionalLight` is a float — the engine
convention is `ambient * color` for the actual ambient term. Match
that.

In `VulkanRenderer::submit`, replace:

```cpp
const Mat4 mvp = pendingProjection_ * pendingView_ * call.model;
const VkDeviceSize uboOffset = frames_.allocateUbo(&mvp, sizeof(Mat4));
```

with:

```cpp
LitUbo ubo;
ubo.mvp      = pendingProjection_ * pendingView_ * call.model;
ubo.model    = call.model;
ubo.sunDir   = Vec4{pendingSunDir_.x, pendingSunDir_.y, pendingSunDir_.z, 0.0f};
ubo.sunColor = Vec4{pendingSunColor_.x, pendingSunColor_.y, pendingSunColor_.z, 0.0f};
ubo.ambient  = Vec4{pendingAmbient_.x, pendingAmbient_.y, pendingAmbient_.z, 0.0f};
ubo.emissive = Vec4{call.material.emissive.x, call.material.emissive.y, call.material.emissive.z, 0.0f};
const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));
```

Update the `VkDescriptorBufferInfo::range` (further down in the same
function) from `sizeof(Mat4)` to `sizeof(ubo)` (= `sizeof(LitUbo)` =
192 bytes).

### Spinning-cube shader update

In `games/01-spinning-cube/main.cpp`, the `#ifdef IRON_RENDER_BACKEND_VULKAN`
vertex + fragment shaders get rewritten:

```glsl
// VERTEX
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
} u;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormal;

void main() {
    vUV = aUV;
    vNormal = mat3(u.model) * aNormal;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
```

```glsl
// FRAGMENT
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
} u;

layout(set = 0, binding = 1) uniform sampler2D uTex;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = -normalize(u.sunDir.xyz);
    float lambert = max(dot(N, L), 0.0);
    vec3 lighting = u.sunColor.xyz * lambert + u.ambient.xyz;
    vec3 diff = texture(uTex, vUV).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
```

### Net-shooter shader update

In `games/07-net-shooter/main.cpp`, the `#ifdef IRON_RENDER_BACKEND_VULKAN`
shaders are rewritten with the same structure as spinning-cube above.
The OpenGL `#else` branch is unchanged (still uses the full lit pass
with per-uniform style).

Update the Vulkan-startup warning string:

```cpp
iron::Log::warn("net-shooter Vulkan path is lit (sun + ambient + "
                "emissive) but lacks point lights, fog, shadows, "
                "cubemap reflections, and normal/spec maps. Full "
                "parity ships in future milestones.");
```

### Asymmetry call-out: `DirectionalLight::ambient`

Looking at the engine convention (`engine/render/Light.h`):
`DirectionalLight` has `direction`, `color`, and `ambient` (a scalar
float). The OpenGL lit shader uses `uAmbient` as a scalar; the actual
ambient term in the lit shader is `uLightColor * uAmbient`. We
replicate the same: store `light.ambient * light.color` as a Vec3 in
`pendingAmbient_` at beginFrame time, so the Vulkan shader's
`u.ambient.xyz` is the final pre-multiplied ambient color.

This keeps the Vulkan shader simpler (no multiplication of two
ambient-related uniforms in the fragment shader) at the cost of one
extra vec3 multiplication per beginFrame on the CPU. Trade-off is
fine — beginFrame happens once per frame.

## Data flow per frame

```
1. game.beginFrame(clear, light, pointLights, fog, view, projection)
   └── VulkanRenderer stores pendingView_, pendingProjection_,
       pendingSunDir_, pendingSunColor_, pendingAmbient_ = ambient*color
       (pointLights + fog ignored under M12)
2. game.submit(call) × N
   └── VulkanRenderer builds LitUbo{mvp, model, sunDir, sunColor,
       ambient, emissive=call.material.emissive}
   └── frames_.allocateUbo(192 bytes) → writes into host-mapped
       per-frame UBO ring at the next 256-aligned offset
   └── descriptor set bound; vkCmdDrawIndexed records
3. ... particles / debug-lines / hud / endFrame as before
```

## Error handling

- The new struct is value-typed and uploaded via memcpy through the
  existing `VkFrameRing::allocateUbo`. No new error paths.
- If a future caller forgets to call `beginFrame` before `submit`,
  the pending fields fall through their constructor defaults
  (sunDir = {0,-1,0}, sunColor = white, ambient = {0.1,0.1,0.1}).
  That gives a sane top-lit fallback rather than NaN'd lighting.

## Testing

### Unit tests

No new unit tests in M12. The change is pipeline + shader work.

### Smoke tests (manual)

After implementation:

- **Spinning-cube on Vulkan:** cube faces show light/dark gradient
  depending on facing relative to the sun. Crate texture still
  visible.
- **Spinning-cube on OpenGL:** unchanged from main (no regression).
- **Net-shooter on Vulkan:** walls show directional shading
  (gradients across faces). Rocket tracer emissive cubes glow
  bright orange. Rocket trail brightness matches the GL build's
  emissive behavior (without the bloom/fog/etc. of the GL path).
- **Net-shooter on OpenGL:** unchanged from main.
- **Particle-storm on Vulkan:** unchanged from main. Its
  `VkParticleSystem` uses its own UBO, not the `submit` path —
  verifies the M12 changes don't leak into compute particles.
- Window title on Vulkan builds still reads `[Vulkan]`.

### CI

Existing CTest run under both `-DIRON_RENDER_BACKEND=opengl` and
`-DIRON_RENDER_BACKEND=vulkan` continues to pass. 35/35 total.

## Risks

- **`call.material.emissive` field type assumption.** The design
  assumes it's a `Vec3`. Confirm during implementation; adjust the
  `Vec4{em.x, em.y, em.z, 0.0f}` initializer if needed. If
  `emissive` doesn't exist on `Material`, the spinning-cube and
  net-shooter `DrawCall` initialization will not set it; default-
  initialized Vec3 (zero) is fine.
- **std140 alignment surprises.** The `LitUbo` struct uses only
  mat4 and vec4 members so std140 rules are trivially satisfied. A
  static_assert on `sizeof(LitUbo) == 192` catches any compiler
  packing surprises.
- **Negative-Y viewport interaction with normals.** M9's Vulkan
  viewport uses negative height to match OpenGL clip-Y. Normals are
  transformed in the vertex shader by `mat3(u.model)` which has no
  view dependency — the negative-height trick doesn't affect lit
  math.

## File / module changes

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h` — adds three
  private pending fields.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — adds
  `LitUbo` struct in anonymous namespace; extends `beginFrame` to
  store sun/ambient; rewrites the UBO upload portion of `submit`;
  updates the `VkDescriptorBufferInfo::range`.
- `games/01-spinning-cube/main.cpp` — replaces the Vulkan-branch
  shaders.
- `games/07-net-shooter/main.cpp` — replaces the Vulkan-branch
  shaders; updates the runtime warning string.
- `docs/engine/rhi-abstraction.md` — appended section on the M12 lit
  UBO + what's still deferred.

### New files

None.

## Open questions

None blocking. Spec ready for implementation plan.
