# M15 — Vulkan point lights + fog

**Status:** design approved 2026-05-26.

**Direction context:** Vulkan parity track. After M14 (shadow map),
M15 adds the remaining UBO-only lit-pass features — up to 16 point
lights with range-based smoothstep falloff (no inverse-square
singularity) and exponential distance fog. Plumbing-only milestone:
shader + UBO + VulkanRenderer plumbing only; no game-side wiring.
Strandbound + showcase + net-tag will pick up these features once
they're ported to Vulkan in their own future milestones.

## Goals

1. Extend `LitUbo` from 288 bytes (M14) to 832 bytes — appends fog,
   pointLightCount, and two parallel arrays of 16 packed
   point-light slots.
2. `VulkanRenderer::beginFrame` already accepts
   `std::span<const PointLight>` and `const Fog&` (currently
   ignored). M15 stores them in new pending state.
3. `VulkanRenderer::recordSceneDraw` packs the pending state into
   the per-draw `LitUbo`.
4. Spinning-cube + net-shooter Vulkan shaders rewritten to compute
   the point-light contribution (matching the OpenGL
   `kMaxPointLights = 16` shader) and fog mix at the end.
5. Net-shooter startup warning updated.

## Scope

### In

- `LitUbo` grows from 288 to 832 bytes (`+544`).
- New private state on `VulkanRenderer`:
  - `std::array<PointLight, kMaxPointLights> pendingPointLights_;`
  - `int pendingPointLightCount_ = 0;`
  - `Fog pendingFog_;` (struct from `engine/render/Fog.h` —
    `Vec3 color; float density;`).
- `beginFrame` populates the new pending state from its existing
  `pointLights` span + `fog` arg (truncating to 16 lights if more
  are passed).
- `recordSceneDraw` packs the state into `LitUbo`.
- Spinning-cube + net-shooter Vulkan shaders gain a point-light
  loop + fog mix.
- Net-shooter startup warning string updated.

### Out

- Cubemap skybox + cubemap reflection (M16).
- Planar reflection (M17).
- Per-axis UV scale (backlog from M13).
- Game-side wiring of actual point lights or non-zero fog density
  (carried into a future milestone or a small follow-up commit when
  a Vulkan-ported game needs them; net-shooter + spinning-cube ship
  with no lights and zero-density fog — the shader code path is
  exercised but visible output is unchanged).
- Splitting per-frame data into its own UBO + descriptor binding
  (proper architecture but adds binding-count restructure; YAGNI
  for now — the redundant per-frame data per draw is ~40 KB at
  net-shooter scale, well within budget).

## Architecture

### Final `LitUbo` (832 bytes)

```cpp
struct LitUbo {
    Mat4 mvp;                 // 64
    Mat4 model;               // 64
    Mat4 lightViewProj;       // 64
    Vec4 sunDir;              // 16
    Vec4 sunColor;            // 16
    Vec4 ambient;             // 16
    Vec4 emissive;            // 16
    Vec4 cameraPos;           // 16
    Vec4 materialParams;      // 16  x=uvScale, y=specPower, z=reflectivity, w=shadowBias
    Vec4 fogColor;            // 16  M15 — xyz=color, w=density
    Vec4 lightCounts;         // 16  M15 — x=pointLightCount (as float), y/z/w padding
    Vec4 pointPositions[16];  // 256 M15 — xyz=position, w=intensity
    Vec4 pointColors[16];     // 256 M15 — xyz=color, w=range
};
static_assert(sizeof(LitUbo) == 832, "LitUbo std140 layout");
```

std140 compliance: all matrices are 64-byte aligned; all vec4s and
vec4 arrays are 16-byte aligned. Array stride = 16 bytes (vec4 is
the natural stride). 288 + 16 + 16 + 256 + 256 = 832 bytes.

Per-draw upload cost: 832 bytes rounds up to 1024 with 256-byte
alignment. Net-shooter ~40 draws × 1024 = 40 KB per frame.
Per-frame UBO sub-allocator budget is 256 KB — comfortable headroom.

### `VulkanRenderer` changes

New private fields alongside the M14 pending state, before the M14
frame-flow state:

```cpp
// M15 — point lights + fog.
std::array<PointLight, kMaxPointLights> pendingPointLights_{};
int  pendingPointLightCount_ = 0;
Fog  pendingFog_{};
```

`#include "render/Light.h"` and `#include "render/Fog.h"` already
come transitively via `Renderer.h`; verify before adding.
`kMaxPointLights = 16` is the existing constant in `Renderer.h`.

In `beginFrame`, after the existing M14
`pendingLightViewProj_ = computeLightViewProj(...)` line, add:

```cpp
pendingPointLightCount_ = static_cast<int>(
    std::min<std::size_t>(pointLights.size(), kMaxPointLights));
for (int i = 0; i < pendingPointLightCount_; ++i) {
    pendingPointLights_[i] = pointLights[i];
}
pendingFog_ = fog;
```

Need `#include <algorithm>` for `std::min` if not already present.

In `recordSceneDraw`, after the existing M14 LitUbo populate block
(which sets `ubo.lightViewProj`, `ubo.materialParams`), add:

```cpp
ubo.fogColor = Vec4{
    pendingFog_.color.x, pendingFog_.color.y, pendingFog_.color.z,
    pendingFog_.density,
};
ubo.lightCounts = Vec4{
    static_cast<float>(pendingPointLightCount_), 0.0f, 0.0f, 0.0f,
};
for (int i = 0; i < pendingPointLightCount_; ++i) {
    const PointLight& pl = pendingPointLights_[i];
    ubo.pointPositions[i] = Vec4{
        pl.position.x, pl.position.y, pl.position.z, pl.intensity,
    };
    ubo.pointColors[i] = Vec4{
        pl.color.x, pl.color.y, pl.color.z, pl.range,
    };
}
// Defensive: zero out unused slots so the GPU never reads garbage.
for (int i = pendingPointLightCount_; i < kMaxPointLights; ++i) {
    ubo.pointPositions[i] = Vec4{0, 0, 0, 0};
    ubo.pointColors[i]    = Vec4{0, 0, 0, 0};
}
```

The defensive zeroing matters because LitUbo is a local in
`recordSceneDraw` — without zeroing, the unused slots would carry
uninitialized stack values. The shader's `for (int i = 0; i < count;)`
loop only reads `count` entries, but better to keep the UBO clean
for debugging.

### Shader updates (spinning-cube + net-shooter, identical)

Vertex shader: outputs unchanged. UBO declaration adds the four new
fields at the end:

```glsl
layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;          // M15
    vec4 lightCounts;       // M15
    vec4 pointPositions[16];// M15
    vec4 pointColors[16];   // M15
} u;
```

Fragment shader: same UBO declaration. After the existing sun
`lighting` computation (sun + shadow + ambient), add the point-light
loop:

```glsl
int plCount = int(u.lightCounts.x);
for (int i = 0; i < plCount; ++i) {
    vec3 toLight = u.pointPositions[i].xyz - vWorldPos;
    float dist  = length(toLight);
    float range = u.pointColors[i].w;
    if (dist < 0.0001 || dist >= range) continue;
    vec3 Lp = toLight / dist;
    float falloff   = 1.0 - smoothstep(0.0, range, dist);
    float intensity = u.pointPositions[i].w;
    float diffusePL = max(dot(perturbedN, Lp), 0.0);
    vec3  Hp        = normalize(Lp + V);
    float specPL    = pow(max(dot(perturbedN, Hp), 0.0), specPower);
    lighting += u.pointColors[i].xyz * intensity * falloff
              * (diffusePL + specPL * specMask);
}
```

Then fog at the end, replacing the final `outColor = vec4(lit, 1.0)`
with:

```glsl
vec3 lit = diff * lighting + u.emissive.xyz;
float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
outColor = vec4(finalColor, 1.0);
```

When `u.fogColor.w` (= density) is zero, `exp(-0 * d) = 1`, so
`fogFactor = 0`, and `mix(lit, color, 0) = lit`. Identical to M14
output. No visual change until a game sets non-zero density.
Same for point lights — `plCount = 0` skips the loop entirely.

### Net-shooter startup warning update

```cpp
iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                "+ normal/spec + shadow + point lights + fog "
                "(Blinn-Phong, 3x3 PCF) lit. Still missing cubemap "
                "reflections. Full parity ships in future milestones.");
```

## Data flow per frame

```
1. game.beginFrame(clear, light, pointLights, fog, view, projection)
   └── VulkanRenderer stores pendingPointLights_ (truncated to 16),
       pendingPointLightCount_, pendingFog_ (in addition to M12-M14 state)
2. game.submit(call) × N
   └── sceneDraws_.push_back(call)
3. game.endFrame()
   └── shadow pass (M14) replays sceneDraws_
   └── scene pass:
       for each call: recordSceneDraw packs LitUbo (now 832 bytes
       with fog + light arrays) + 5 descriptor writes
       deferred callbacks + debug-lines + HUD as before
```

## Error handling

- Excess point lights silently dropped at `beginFrame` (existing GL
  behavior). No log spam — games can pass any number.
- `pendingPointLightCount_` overflow guarded by
  `std::min(..., kMaxPointLights)`.
- Defensive zeroing of unused light slots in `recordSceneDraw` avoids
  reading uninitialized stack memory if a shader were ever to read
  past `lightCounts.x`.
- Zero-density fog is a no-op (intentional).

## Testing

### Unit tests

No new unit tests. UBO + shader plumbing only.

### Smoke tests (manual)

After implementation:

- **Spinning-cube on Vulkan**: visually identical to M14 (no lights,
  zero fog density).
- **Spinning-cube on OpenGL**: unchanged.
- **Net-shooter on Vulkan**: visually identical to M14 (no lights,
  zero fog density). The new shader code paths run but produce no
  visible delta.
- **Net-shooter on OpenGL**: unchanged.
- **Particle-storm on Vulkan**: unchanged (own pipeline).
- Vulkan validation layers run clean.

### CI

35/35 tests pass on both backends.

## Risks

- **UBO size and per-draw upload cost.** 832 bytes × 40 draws =
  ~33 KB per frame. Within the 256 KB per-frame UBO budget. Comfortable.
- **std140 alignment of vec4 arrays.** A `Vec4[16]` array in std140
  has a 16-byte stride (each element). The `static_assert(sizeof
  (LitUbo) == 832)` catches any compiler packing surprises.
- **Float-to-int cast for `lightCounts.x`.** `int(u.lightCounts.x)`
  in GLSL is well-defined for non-negative values up to 2^24. The
  cap is 16. Safe.
- **Game-side discoverability.** No game currently uses
  `PointLight` or `Fog` on Vulkan, so the new code paths are
  silent. A future user porting strandbound to Vulkan will get them
  for free without touching engine code.

## File / module changes

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h` — adds
  `pendingPointLights_`, `pendingPointLightCount_`, `pendingFog_`
  private fields.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — extends
  `LitUbo` to 832 bytes; `beginFrame` stores point lights + fog;
  `recordSceneDraw` packs them into the UBO.
- `games/01-spinning-cube/main.cpp` — Vulkan-branch shaders gain
  the new UBO fields, point-light loop, fog mix.
- `games/07-net-shooter/main.cpp` — same; warning string updated.
- `docs/engine/rhi-abstraction.md` — appended M15 section.

### New files

None.

## Open questions

None blocking.
