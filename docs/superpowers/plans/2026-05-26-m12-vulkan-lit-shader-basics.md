# M12 Vulkan Lit Shader Basics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `VulkanRenderer::submit` to upload a richer per-draw UBO so Vulkan games can render with directional sun + ambient + per-draw emissive instead of being stuck at unlit-textured.

**Architecture:** A new `LitUbo` struct (192 bytes, std140-safe) replaces the bare `Mat4` that `submit` uploads today. `beginFrame` stores the directional light + ambient from its existing args; `submit` packs everything (mvp + model + sun + ambient + emissive) into the per-draw UBO. Spinning-cube and net-shooter's Vulkan-branch shaders are rewritten to consume the new UBO. The descriptor set layout (set=0 binding=0 = UBO, binding=1 = sampler) is unchanged — only the UBO's contents grow.

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), MSVC, CMake.

---

## File Structure

### Modified files
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds three private pending fields (`pendingSunDir_`, `pendingSunColor_`, `pendingAmbient_`)
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — adds `LitUbo` struct in anonymous namespace; extends `beginFrame`; rewrites the UBO-upload portion of `submit`; updates `VkDescriptorBufferInfo::range`
- `games/01-spinning-cube/main.cpp` — replaces the Vulkan-branch shaders with lit versions
- `games/07-net-shooter/main.cpp` — same, plus updates the runtime warning string
- `docs/engine/rhi-abstraction.md` — appended section on the M12 lit UBO

### New files
None.

---

## Task 1: VulkanRenderer — LitUbo + submit upload + beginFrame fields

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`

This task is engine-only. It's intentionally backward-compatible with the existing M11 Vulkan shaders: those shaders read `mat4 uMvp` from the first 64 bytes of the UBO, and `LitUbo::mvp` is the first member. So after this task lands, spinning-cube and net-shooter on Vulkan still render exactly as before — they just upload 192 bytes per draw instead of 64. Shader updates land in Tasks 2 and 3.

- [ ] **Step 1: Add private fields to `VulkanRenderer.h`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, find the existing pending-state block (around `pendingClear_`, `pendingView_`, `pendingProjection_` — currently a few lines above `currentImageIndex_`). Add three new fields immediately after `pendingProjection_`:

```cpp
    // M12 — directional light + ambient stored at beginFrame, packed
    // into each draw's LitUbo by submit.
    Vec3 pendingSunDir_   = {0.0f, -1.0f, 0.0f};
    Vec3 pendingSunColor_ = {1.0f, 1.0f, 1.0f};
    Vec3 pendingAmbient_  = {0.1f, 0.1f, 0.1f};
```

- [ ] **Step 2: Extend `beginFrame` to populate the new fields**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, find `VulkanRenderer::beginFrame`. Locate the line that assigns `pendingProjection_ = projection;` (or the equivalent existing line that stores the projection). Immediately after that line, add:

```cpp
    pendingSunDir_   = light.direction;
    pendingSunColor_ = light.color;
    pendingAmbient_  = Vec3{light.ambient * light.color.x,
                            light.ambient * light.color.y,
                            light.ambient * light.color.z};
```

`light.ambient` is a scalar float (verified — `engine/render/Light.h` line 12). The pre-multiplication mirrors what the OpenGL lit shader does (`uLightColor * uAmbient`), so the Vulkan fragment shader's `u.ambient.xyz` is the final pre-multiplied ambient color.

- [ ] **Step 3: Add the `LitUbo` struct to `VulkanRenderer.cpp`**

Near the top of `engine/render/backends/vulkan/VulkanRenderer.cpp`, in an anonymous namespace (creating one if not already present — check just under the existing `#include` block), add:

```cpp
namespace {

// M12 — per-draw UBO uploaded by submit. std140 layout: all members
// are mat4 (64-byte aligned) or vec4 (16-byte aligned), so no
// straddling. Total 192 bytes.
struct LitUbo {
    Mat4 mvp;        // 64 — projection * view * model
    Mat4 model;      // 64 — for mat3(model) * aNormal in the vertex shader
    Vec4 sunDir;     // 16 — xyz direction; w padding
    Vec4 sunColor;   // 16 — xyz color; w padding
    Vec4 ambient;    // 16 — xyz color (pre-multiplied); w padding
    Vec4 emissive;   // 16 — xyz color (from call.material.emissive); w padding
};
static_assert(sizeof(LitUbo) == 192, "LitUbo std140 layout");

}  // namespace
```

If the file already has an anonymous namespace at the top, add the struct + static_assert inside it. If not, create one.

- [ ] **Step 4: Rewrite the UBO-upload portion of `submit`**

In the same file, find `VulkanRenderer::submit`. Locate these two lines:

```cpp
    const Mat4 mvp = pendingProjection_ * pendingView_ * call.model;
    const VkDeviceSize uboOffset = frames_.allocateUbo(&mvp, sizeof(Mat4));
```

Replace them with:

```cpp
    LitUbo ubo;
    ubo.mvp      = pendingProjection_ * pendingView_ * call.model;
    ubo.model    = call.model;
    ubo.sunDir   = Vec4{pendingSunDir_.x,   pendingSunDir_.y,   pendingSunDir_.z,   0.0f};
    ubo.sunColor = Vec4{pendingSunColor_.x, pendingSunColor_.y, pendingSunColor_.z, 0.0f};
    ubo.ambient  = Vec4{pendingAmbient_.x,  pendingAmbient_.y,  pendingAmbient_.z,  0.0f};
    ubo.emissive = Vec4{call.material.emissive.x,
                        call.material.emissive.y,
                        call.material.emissive.z,
                        0.0f};
    const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));
```

Further down in the same function, find the `VkDescriptorBufferInfo` block. It currently has `bufInfo.range = sizeof(Mat4);`. Update to:

```cpp
    bufInfo.range = sizeof(ubo);
```

(`sizeof(ubo)` reads cleaner than the equivalent `sizeof(LitUbo)` since `ubo` is the local variable in scope.)

- [ ] **Step 5: Build under both backends + run tests**

Run:

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both backends.

- [ ] **Step 6: Smoke-test the Vulkan builds**

Launch `build-vk/games/01-spinning-cube/Debug/01-spinning-cube.exe`. Expected: cube rotates, textured. Same as before M12 — Task 1 alone doesn't change the visual output (shaders still read only `mat4 mvp` from the start of the bigger UBO).

Launch `build-vk/games/07-net-shooter/Debug/07-net-shooter.exe --listen`. Expected: same flat-shaded scene as M11. HUD + gizmos still work.

If either smoke test crashes or renders all black, investigate. Most likely cause would be a UBO-range mismatch (descriptor write uses `sizeof(ubo)` but the buffer's actual host-mapped region is smaller than expected — but `VkFrameRing::allocateUbo` reserves the requested bytes, so this should not fire).

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M12 Task 1: VulkanRenderer LitUbo + submit upload + beginFrame fields"
```

---

## Task 2: Spinning-cube Vulkan shader rewrite

**Files:**
- Modify: `games/01-spinning-cube/main.cpp`

- [ ] **Step 1: Locate the Vulkan-branch shaders**

In `games/01-spinning-cube/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` branch around line 14. It currently contains:

```cpp
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform Ubo { mat4 uMvp; } ubo;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = ubo.uMvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV);
}
)";
```

- [ ] **Step 2: Replace with the lit versions**

Replace the entire `#ifdef IRON_RENDER_BACKEND_VULKAN` shader block with:

```cpp
const char* kVertexShader = R"(#version 450
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
)";

const char* kFragmentShader = R"(#version 450
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
)";
```

The OpenGL `#else` branch is unchanged.

- [ ] **Step 3: Build Vulkan + smoke-test**

```
cmake --build build-vk --config Debug
```

Expected: clean build. If the shader compile fails at runtime (glslang error visible in console), inspect the error message — most likely a typo in the GLSL source.

Launch `build-vk/games/01-spinning-cube/Debug/01-spinning-cube.exe`. Expected:
- Cube renders with **visible facets**: the side facing the sun is brightest, opposite side is dim (only ambient lighting), other sides have gradient.
- As the cube rotates, the shaded sides shift.
- Crate texture is still visible.

If the cube looks pure black, the issue is most likely:
- `light.direction` from spinning-cube's beginFrame is zero (would mean Lambertian = 0 everywhere). Check spinning-cube's `beginFrame` args.
- Or `light.color` is zero. Same.
- Or normals aren't being transformed correctly (`vNormal` is zero or NaN). Check that `aNormal` is uploaded by `engine/scene/Mesh` for the cube.

- [ ] **Step 4: Build OpenGL + verify no regression**

```
cmake --build build --config Debug
```

Launch `build/games/01-spinning-cube/Debug/01-spinning-cube.exe`. Expected: identical to before M12 (OpenGL path is unchanged).

- [ ] **Step 5: Run full test suite on both backends**

```
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 6: Commit**

```bash
git add games/01-spinning-cube/main.cpp
git commit -m "M12 Task 2: spinning-cube Vulkan shader uses LitUbo (Lambertian + ambient + emissive)"
```

---

## Task 3: Net-shooter Vulkan shader rewrite + warning update

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Locate the Vulkan-branch shaders**

In `games/07-net-shooter/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` block (added in M11). It contains the same single-mat4 UBO shader as spinning-cube did before Task 2.

- [ ] **Step 2: Replace with the lit versions**

Replace the entire `#ifdef IRON_RENDER_BACKEND_VULKAN` shader block (just the Vulkan branch — leave the `#else` OpenGL block untouched) with:

```cpp
const char* kVertexShader = R"(#version 450
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
)";

const char* kFragmentShader = R"(#version 450
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
)";
```

- [ ] **Step 3: Update the one-time startup warning**

Find the existing block in net-shooter's `main()` that contains:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path is unlit textured "
                    "(no lighting / shadows / cubemap / reflection / fog / emissive). "
                    "Full lit-shader parity ships in a later milestone.");
#endif
```

Replace the `Log::warn` call with:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive lit. "
                    "Still missing point lights, fog, shadows, cubemap "
                    "reflections, and normal/spec maps. Full parity ships "
                    "in future milestones.");
#endif
```

- [ ] **Step 4: Build Vulkan + smoke-test (host only)**

```
cmake --build build-vk --config Debug
```

Launch `build-vk/games/07-net-shooter/Debug/07-net-shooter.exe --listen`. Expected:
- Walls show directional shading (gradients across faces depending on sun angle).
- Floor still shows grass texture, now with proper shading.
- Rocket tracer emissive cubes glow bright orange (they have non-zero `material.emissive`).
- HUD + gizmos still work (M11 features unaffected).
- PowerShell console shows the new warning string about "sun + ambient + emissive lit".

If the scene is still pure white walls (like M11 was), the most likely cause is `light.direction` being zero or the beginFrame light args not being set in net-shooter. Check the existing beginFrame call site in net-shooter and confirm a `DirectionalLight` with non-zero direction is being passed.

- [ ] **Step 5: Build OpenGL + verify no regression**

```
cmake --build build --config Debug
```

Launch `build/games/07-net-shooter/Debug/07-net-shooter.exe --listen`. Expected: identical to M11 OpenGL net-shooter (with the full GL lit shader — shadows, point lights, fog, etc.).

- [ ] **Step 6: Run full test suite on both backends**

```
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 7: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M12 Task 3: net-shooter Vulkan shader uses LitUbo + updated warning"
```

---

## Task 4: Docs append

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M12 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
## Vulkan lit shader basics (M12)

The Vulkan backend's per-draw UBO grew from a bare `mat4 mvp` to a
`LitUbo` struct (192 bytes, std140-safe):

```cpp
struct LitUbo {
    Mat4 mvp;        // projection * view * model
    Mat4 model;      // for mat3(model) * aNormal in the vertex shader
    Vec4 sunDir;     // xyz direction; w padding
    Vec4 sunColor;   // xyz color; w padding
    Vec4 ambient;    // xyz pre-multiplied ambient color; w padding
    Vec4 emissive;   // xyz from call.material.emissive; w padding
};
```

`VulkanRenderer::beginFrame` stores the directional light + ambient
from its args; `VulkanRenderer::submit` packs everything into
`LitUbo` and uploads via `VkFrameRing::allocateUbo`. The descriptor
set layout (set=0 binding=0 = UBO, binding=1 = combined image
sampler) is unchanged — only the UBO contents grew.

Vulkan shaders in `games/01-spinning-cube/main.cpp` and
`games/07-net-shooter/main.cpp` were rewritten to consume `LitUbo`
and perform Lambertian shading: `dot(N, -sunDir) * sunColor +
ambient`, multiplied by the diffuse texture sample, plus the
per-draw emissive.

### What's still missing on the Vulkan lit path

These all need additional UBO fields and either pipeline state
(LEQUAL depth compare, extra subpasses) or whole new passes
(shadow depth, cubemap, reflection RTT):

- Point lights (16-array with range falloff).
- Exponential distance fog.
- Shadow map sampling (needs the depth-pass port).
- Cubemap skybox + cubemap-based reflection sampling.
- Planar reflection sampling (needs RTT pipeline).
- Normal maps + specular maps + TBN math.
- Per-DrawCall UV scale (`call.material.uvScale`).

Net-shooter's Vulkan-startup warning enumerates the gaps so the user
knows what to expect.
```

- [ ] **Step 2: Run full test suite on both backends to confirm nothing got disturbed**

```
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 3: Commit**

```bash
git add docs/engine/rhi-abstraction.md
git commit -m "M12 Task 4: docs — Vulkan lit shader basics"
```

---

## Final verification

- [ ] **Step 1: Full test suite on both backends**

```
ctest --test-dir build    -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 2: Manual smoke matrix**

| Game | Backend | Expected |
|------|---------|----------|
| `01-spinning-cube` | OpenGL | unchanged |
| `01-spinning-cube` | Vulkan | NOW lit (Lambertian shading on rotating cube) |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | NOW lit (directional sun shading on walls, emissive tracers glow) |
| `08-particle-storm` | Vulkan | unchanged (its compute pipeline doesn't use the LitUbo path) |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m12-vulkan-lit-shader-basics
gh pr create --title "M12: Vulkan lit shader basics (sun + ambient + emissive)" --body "..."
```

PR body should summarize the LitUbo extension, the two shaders rewritten, and the manual smoke outcomes (with screenshots if practical).

---

## Self-review notes

- **Spec coverage:** every "In scope" item from the spec maps to a task. The new `LitUbo` (spec §Architecture) lands in Task 1 Step 3. The `pendingSunDir_/Color_/Ambient_` fields (spec §VulkanRenderer changes) land in Task 1 Step 1+2. The submit UBO rewrite (spec §VulkanRenderer changes) lands in Task 1 Step 4. Spinning-cube shader rewrite (spec §Spinning-cube shader update) is Task 2. Net-shooter shader rewrite + warning (spec §Net-shooter shader update) is Task 3. Docs append is Task 4.
- **No placeholders:** every code block contains the actual content. No TBD / TODO / "implement later".
- **Type consistency:** `LitUbo` struct field names (`mvp`, `model`, `sunDir`, `sunColor`, `ambient`, `emissive`) match across all tasks and all referenced shaders. The `static_assert(sizeof(LitUbo) == 192)` guard is in Task 1 Step 3 and is the single source of truth for the size.
- **One pre-verified field-type assumption:** `DirectionalLight::ambient` is `float` (confirmed in `engine/render/Light.h`), `Material::emissive` is `Vec3` (confirmed in `engine/render/Material.h`). The plan's code is consistent with both.
