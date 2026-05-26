# M15 Vulkan Point Lights + Fog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plumb directional point lights (up to 16, range-based falloff) and exponential distance fog through the Vulkan lit pass — pure UBO + shader extension, no new render passes or pipelines.

**Architecture:** Extend `LitUbo` from 288 to 832 bytes with `fogColor` + `lightCounts` + two parallel `Vec4[16]` arrays (positions+intensity / colors+range). `VulkanRenderer::beginFrame` stores the existing-but-ignored `pointLights` + `fog` args; `recordSceneDraw` packs them into the UBO. Spinning-cube + net-shooter Vulkan shaders gain a point-light loop with `1 - smoothstep(0, range, dist)` falloff and a fog mix `1 - exp(-density * distance)` at the end.

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), MSVC, CMake.

---

## File Structure

### Modified files
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds `pendingPointLights_` array + `pendingPointLightCount_` int + `pendingFog_` struct
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — extends `LitUbo` to 832 bytes; extends `beginFrame` to store the new state; extends `recordSceneDraw` to pack lights + fog into the UBO
- `games/01-spinning-cube/main.cpp` — Vulkan shaders extended with point-light loop + fog mix + new UBO fields
- `games/07-net-shooter/main.cpp` — same; warning string updated
- `docs/engine/rhi-abstraction.md` — appended M15 section

### New files
None.

---

## Task 1: VulkanRenderer plumbing (LitUbo + pending state + recordSceneDraw)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`

This task is engine-only. The shader still reads only the first 288 bytes of the UBO (M14 layout), so visually nothing changes. Task 2 expands the shaders.

- [ ] **Step 1: Add `pendingPointLights_` / `pendingPointLightCount_` / `pendingFog_` to `VulkanRenderer.h`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, find the M14 shadow-state block (last field is `VkShadowMap shadowMap_;`). Add immediately after it:

```cpp
    // M15 — point lights + fog (existing beginFrame args, now stored).
    std::array<PointLight, kMaxPointLights> pendingPointLights_{};
    int  pendingPointLightCount_ = 0;
    Fog  pendingFog_{};
```

Add the include for `<array>` if not already present (verify with Read). `PointLight`, `Fog`, and `kMaxPointLights` come transitively from `Renderer.h` which is already included — no new includes for them.

- [ ] **Step 2: Extend the `LitUbo` struct in `VulkanRenderer.cpp`**

Find the M14 `LitUbo` struct in the anonymous namespace. Replace with:

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

- [ ] **Step 3: Extend `beginFrame` to store pending point lights + fog**

In `engine/render/backends/vulkan/VulkanRenderer.cpp::beginFrame`, after the M14 line
`pendingLightViewProj_ = computeLightViewProj(pendingSunDir_, pendingShadowCenter_, pendingShadowRadius_);`,
add:

```cpp
    pendingPointLightCount_ = static_cast<int>(
        std::min<std::size_t>(pointLights.size(), kMaxPointLights));
    for (int i = 0; i < pendingPointLightCount_; ++i) {
        pendingPointLights_[i] = pointLights[i];
    }
    pendingFog_ = fog;
```

`pointLights` and `fog` are the existing `beginFrame` args (currently ignored). Add `#include <algorithm>` at the top of the file if not already present (verify with Read; `std::min` is the only consumer).

- [ ] **Step 4: Extend `recordSceneDraw` to pack the new LitUbo fields**

In `engine/render/backends/vulkan/VulkanRenderer.cpp::recordSceneDraw`, after the existing M14 `LitUbo ubo;` populate block (the last assignment was
`ubo.materialParams = Vec4{call.material.uvScale, call.material.specPower, call.material.reflectivity, pendingShadowBias_};`), add:

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
    // Zero unused slots so the GPU never reads uninitialized stack data.
    for (int i = pendingPointLightCount_; i < kMaxPointLights; ++i) {
        ubo.pointPositions[i] = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
        ubo.pointColors[i]    = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
    }
```

This is added AFTER the M14 populate block but BEFORE the `frames_.allocateUbo(&ubo, sizeof(ubo))` call. `sizeof(ubo)` will naturally become 832; the `VkDescriptorBufferInfo::range = sizeof(ubo)` from M14 still works.

- [ ] **Step 5: Build under both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both backends. Visual output identical to M14 because shaders still read only the first 288 bytes of the UBO — the new 544 bytes are ignored.

- [ ] **Step 6: Smoke-test the Vulkan builds (just to confirm no crash)**

```
.\build-vk\games\01-spinning-cube\Debug\01-spinning-cube.exe
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Both should launch and run without crashing. Visual output unchanged.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M15 Task 1: VulkanRenderer plumbing for point lights + fog (LitUbo to 832 bytes)"
```

---

## Task 2: Shader rewrites + warning update (atomic — both games together)

**Files:**
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/07-net-shooter/main.cpp`

Both games share identical Vulkan-branch shaders, so update them together. After this task, the new shader code paths run but produce no visible change for net-shooter / spinning-cube (neither defines point lights, both use density=0 fog).

- [ ] **Step 1: Locate the spinning-cube Vulkan shader block**

In `games/01-spinning-cube/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` block. The current shaders are the M14 versions with TBN + PCF shadow.

- [ ] **Step 2: Replace the spinning-cube Vulkan shaders**

Replace the entire `#ifdef IRON_RENDER_BACKEND_VULKAN` block with:

```cpp
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

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
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

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
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    float bias      = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3 L = -normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse  = max(dot(perturbedN, L), 0.0);
    float spec     = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;
    float shadow   = shadowFactor(vLightSpacePos, bias);

    vec3 lighting = u.sunColor.xyz * (diffuse * shadow + spec * specMask * shadow)
                  + u.ambient.xyz;

    // M15 — point lights.
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

    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;

    // M15 — fog. Zero density = no-op.
    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
}
)";
```

Leave the OpenGL `#else` branch untouched.

- [ ] **Step 3: Replace the net-shooter Vulkan shaders with the EXACT SAME code**

In `games/07-net-shooter/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` block. Replace BOTH `kVertexShader` and `kFragmentShader` strings with the same code from Step 2 (the two games share identical Vulkan shaders).

Leave the OpenGL `#else` branch untouched.

- [ ] **Step 4: Update net-shooter's startup warning**

In `games/07-net-shooter/main.cpp`, find the existing block:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                    "+ normal/spec + shadow map (Blinn-Phong, 3x3 PCF) lit. "
                    "Still missing point lights, fog, cubemap reflections. "
                    "Full parity ships in future milestones.");
#endif
```

Replace the warning string with:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                    "+ normal/spec + shadow + point lights + fog "
                    "(Blinn-Phong, 3x3 PCF) lit. Still missing cubemap "
                    "reflections. Full parity ships in future milestones.");
#endif
```

- [ ] **Step 5: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both. If compilation fails on the shader (glslang error at runtime — caught by `test_glsl_to_spirv`), most likely cause is a typo in the UBO field declaration. Compare to the C++ `LitUbo` struct field-by-field.

- [ ] **Step 6: Smoke-test both Vulkan games**

```
.\build-vk\games\01-spinning-cube\Debug\01-spinning-cube.exe
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Each should launch and render. Visual output is unchanged from M14 (no game defines point lights; fog density is zero). The PowerShell log should show the new warning string for net-shooter.

- [ ] **Step 7: Commit (atomic — both games + warning in one commit)**

```bash
git add games/01-spinning-cube/main.cpp games/07-net-shooter/main.cpp
git commit -m "M15 Task 2: Vulkan shaders — point-light loop + fog mix + warning update"
```

---

## Task 3: Docs append

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M15 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
## Vulkan point lights + fog (M15)

Plumbing-only milestone — extends the Vulkan lit pass with the
remaining UBO-only features. No new render passes, no new
pipelines, no new descriptor bindings.

### LitUbo grew to 832 bytes

```cpp
struct LitUbo {
    Mat4 mvp;
    Mat4 model;
    Mat4 lightViewProj;
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 emissive;
    Vec4 cameraPos;
    Vec4 materialParams;     // x=uvScale, y=specPower, z=reflectivity, w=shadowBias
    Vec4 fogColor;           // M15 — xyz=color, w=density
    Vec4 lightCounts;        // M15 — x=pointLightCount (as float)
    Vec4 pointPositions[16]; // M15 — xyz=position, w=intensity
    Vec4 pointColors[16];    // M15 — xyz=color, w=range
};
```

Each point light is packed across two parallel array slots so the
std140 stride remains a clean 16 bytes. The `kMaxPointLights = 16`
constant matches the OpenGL backend. Excess lights are silently
dropped at `beginFrame`.

`VulkanRenderer::beginFrame` already accepts `std::span<const
PointLight>` and `const Fog&` as args (existing since M9 — were
silently ignored until now). M15 stores them in `pendingPointLights_`
+ `pendingPointLightCount_` + `pendingFog_`; `recordSceneDraw` packs
them into the UBO.

### Shader-side point light + fog

The fragment shader's existing sun + shadow + ambient lighting is
unchanged. After that block, a `for (int i = 0; i < count; ++i)`
loop adds each point light's contribution: `1 - smoothstep(0, range,
dist)` falloff (no inverse-square singularity), Lambertian +
Blinn-Phong via the perturbed normal from the M13 TBN.

Fog mix happens at the very end:
`finalColor = mix(lit, fogColor.xyz, clamp(1 - exp(-density * d), 0, 1))`
where `d = length(cameraPos - vWorldPos)`. When density = 0,
fogFactor = 0, mix is a no-op.

### Per-draw upload cost

832 bytes per draw rounds to 1024 with 256-byte UBO alignment. At
net-shooter's ~40 draws that's 40 KB per frame, well within the
256 KB per-frame UBO sub-allocator budget.

### What's still missing

- Cubemap skybox + cubemap reflection — M16.
- Planar reflection — M17.

After M16-M17 land, the Vulkan backend reaches full parity with the
OpenGL lit pass.
```

- [ ] **Step 2: Run full test suite on both backends**

```
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 3: Commit**

```bash
git add docs/engine/rhi-abstraction.md
git commit -m "M15 Task 3: docs — Vulkan point lights + fog"
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
| `01-spinning-cube` | Vulkan | unchanged (no lights, density 0) |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | unchanged (no lights, density 0) — new warning string in console |
| `08-particle-storm` | Vulkan | unchanged |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m15-vulkan-point-lights-fog
gh pr create --title "M15: Vulkan point lights + fog" --body "..."
```

---

## Self-review notes

- **Spec coverage:** every "In scope" item from the spec maps to a task. LitUbo growth (spec §LitUbo) is Task 1 Step 2. Pending state (spec §VulkanRenderer changes) is Task 1 Step 1. beginFrame populate (spec §VulkanRenderer changes) is Task 1 Step 3. recordSceneDraw populate (spec §VulkanRenderer changes) is Task 1 Step 4. Shader rewrites + point light loop + fog mix (spec §Shader updates) are Task 2 Steps 2-3. Warning update (spec §Net-shooter startup warning update) is Task 2 Step 4. Docs append is Task 3.
- **No placeholders:** every code block contains the actual content. Inline GLSL, complete C++ struct, complete pending-state populate.
- **Type consistency:** `LitUbo` field order matches between C++ struct (Task 1 Step 2), spinning-cube shader (Task 2 Step 2), and net-shooter shader (Task 2 Step 3). `pendingPointLights_` / `pendingPointLightCount_` / `pendingFog_` named consistently across declaration (Task 1 Step 1) and use (Task 1 Steps 3-4).
- **Backward compatibility within Task 1:** the M14 shaders still work after Task 1 because they read only the first 288 bytes of the (now 832-byte) UBO. This lets Task 1 land independently of Task 2.
