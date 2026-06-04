# M50b Hardware Tessellation + Displacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the engine its first hardware-tessellation pipeline, displace real geometry from the M50a height map, complete the 3-way (flat/POM/tessellated) demo, and add a global wireframe debug toggle.

**Architecture:** A dedicated 4-stage tessellation pipeline (vert→tesc→tese→frag, `PATCH_LIST`, `patchControlPoints=3`) that reuses the existing lit descriptor layout + lit fragment shader. A fixed (UI-tunable) tessellation factor; the `tese` samples the height map and displaces along the normal. Displacement scale + tess factor ride in spare `LitUbo` slots (`reflectionParams.w`, `probeBoxMin.w`) so no UBO growth. Wireframe = a `VK_POLYGON_MODE_LINE` pipeline variant toggled by `setWireframe`.

**Tech Stack:** C++17, Vulkan, glslang, ImGui, CTest. Branched from `main` (`f566791`) — has M50a height map (binding 13) + `heightScale`, M49 `LitUbo` (1008 bytes).

---

## Background: exact code this plan builds on

- **`VkShader`** (`VkShader.h:16-21`): `struct VkShader { VkShaderModule vertexModule, fragmentModule; VkDescriptorSetLayout setLayout; VkPipelineLayout pipelineLayout; };`. Pipelines are NOT owned here — cached in `VkPipeline::pipelines_` keyed by `const VkShader*`.
- **`compileGlsl` stage map** (`VkShader.cpp:26-33`): `toLang()` switch over VERTEX/FRAGMENT/COMPUTE → `EShLang...`.
- **`VkShaderStore::create(ctx, vertSrc, fragSrc)`** (`VkShader.cpp:69-169`): compiles vert+frag, `makeModule` lambda, builds descriptor set layout + pipeline layout, stores `VkShader` in `shaders_`.
- **Pipeline factory** (`VkPipeline.cpp:79-155` `createGraphicsPipelineImpl`): `VkPipelineShaderStageCreateInfo stages[2]` (vert+frag), `ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`, `rs.polygonMode = VK_POLYGON_MODE_FILL`, `info.stageCount = 2`, no `pTessellationState`. Cache: `pipelineFor(ctx, swap, sh)` (`:255-265`) linear-searches `std::vector<std::pair<const VkShader*, ::VkPipeline>> pipelines_`, builds against `scenePass_`.
- **Device features** (`VkContext.cpp:207-224`): `VkPhysicalDeviceFeatures supported{}` (queried) + `features{}` with `shaderClipDistance=VK_TRUE`, `wideLines` (guarded by `supported.wideLines`), `info.pEnabledFeatures = &features`.
- **Lit vert varyings** (`StandardLitShader.h:14-60`): inputs loc 0-3 (aPos/aNormal/aUV/aTangent); outputs loc 0-4 (`vWorldPos`,`vNormal`,`vTangent`,`vUV`,`vLightSpacePos`); LitUbo has `materialParams` (`.x=uvScale`), `model`, `mvp`, `lightViewProj`, `reflectionParams` (`.w=0`, spare), `baseColorFactor` (`.w=heightScale`, M50a), `probeBoxMin` (`.w` unused, M49). The frag does `uv = vUV * materialParams.x`.
- **Renderer**: `createStandardLitShader()` (`Renderer.h:118-126`) → `createShader(vertSrc, fragSrc)`. `recordSceneDraw` (`VulkanRenderer.cpp:596-678`): `const VkShader& sh = shaders_.get(call.shader); ::VkPipeline pipe = pipelines_.pipelineFor(context_, swapchain_, sh); vkCmdBindPipeline(...)`. Pending-flag pattern: `setShadowBounds()` → `pendingShadowCenter_`. `setWireframe` follows this.
- **Sandbox**: `litShader = renderer.createStandardLitShader()` (`main.cpp:173-177`); F-key handling `input.keyPressed(GLFW_KEY_F5)` (`:870-882`).
- **Spare LitUbo slots** used by this plan: `reflectionParams.w` = displacement scale (tess draws); `probeBoxMin.w` = tessellation factor. Both currently 0/unused in the scene pass.

## File structure

**Modify:**
- `engine/render/backends/vulkan/VkContext.cpp` — enable `tessellationShader` + `fillModeNonSolid`.
- `engine/render/backends/vulkan/VkShader.h` / `.cpp` — tesc/tese in `toLang`; `tescModule`/`teseModule` on `VkShader`; a `createTessellated(...)` store method building a TESE-aware layout.
- `engine/render/backends/vulkan/VkPipeline.h` / `.cpp` — 4-stage patch-list tess variant + `VK_POLYGON_MODE_LINE` variant; cache keyed by `(shader, wireframe)`.
- `engine/render/StandardLitShader.h` — `standardLitTessVertSource()`, `standardLitTescSource()`, `standardLitTeseSource()` (reuse lit frag).
- `engine/render/Renderer.h` — `createStandardTessellatedLitShader()`, `createTessellatedShader(...)`, `setWireframe(bool)`, `setTessellationFactor(float)` (+ Vulkan impls, OpenGL/Mock stubs).
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — pending wireframe + tess factor; tess shader create; per-draw pipeline selection (tess + wireframe) + set displacement/factor/POM-off for tess draws.
- `games/11-sandbox/main.cpp` — tessellated quad + labels; F2 wireframe toggle; tess-factor slider.

---

## Task 1: Device features + tesc/tese stage compilation

**Files:** Modify `engine/render/backends/vulkan/VkContext.cpp`, `engine/render/backends/vulkan/VkShader.cpp`.

- [ ] **Step 1: Enable tessellation + fill-mode-non-solid features**

In `VkContext.cpp` (after `features.shaderClipDistance = VK_TRUE;` ~line 211), add (guarded against `supported`, log if absent):
```cpp
    if (supported.tessellationShader != VK_TRUE)
        Log::error("VkContext: tessellationShader feature unsupported — M50b tessellation will fail");
    features.tessellationShader = supported.tessellationShader;
    if (supported.fillModeNonSolid != VK_TRUE)
        Log::warn("VkContext: fillModeNonSolid unsupported — wireframe toggle will be a no-op");
    features.fillModeNonSolid = supported.fillModeNonSolid;
```
(Confirm `Log` is included in VkContext.cpp; it is — used elsewhere.)

- [ ] **Step 2: Add tesc/tese to the glslang stage map**

In `VkShader.cpp` `toLang()` (~line 26-33), add before `default:`:
```cpp
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return EShLangTessControl;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return EShLangTessEvaluation;
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkContext.cpp engine/render/backends/vulkan/VkShader.cpp
git commit -m "M50b: enable tessellationShader + fillModeNonSolid; tesc/tese glslang stages"
```

---

## Task 2: VkShader tessellation modules + tessellated create path

**Files:** Modify `engine/render/backends/vulkan/VkShader.h`, `engine/render/backends/vulkan/VkShader.cpp`.

- [ ] **Step 1: Add tess modules to `VkShader`**

In `VkShader.h`, extend the struct:
```cpp
struct VkShader {
    VkShaderModule        vertexModule    = VK_NULL_HANDLE;
    VkShaderModule        fragmentModule  = VK_NULL_HANDLE;
    VkShaderModule        tescModule      = VK_NULL_HANDLE;  // M50b — tessellation control (optional)
    VkShaderModule        teseModule      = VK_NULL_HANDLE;  // M50b — tessellation evaluation (optional)
    VkDescriptorSetLayout setLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout  = VK_NULL_HANDLE;
};
```
A shader with `tescModule != VK_NULL_HANDLE` is a tessellation shader (used by the pipeline factory in Task 3).

- [ ] **Step 2: Declare a tessellated create method on the store**

In `VkShader.h`, in `VkShaderStore`, declare (next to the existing `create`):
```cpp
    // M50b — tessellated variant: vert + tesc + tese + frag. Builds the SAME
    // lit descriptor layout as the standard lit shader but adds the
    // tessellation-evaluation stage to the UBO (binding 0) + height map
    // (binding 13) so the tese can read them.
    ShaderHandle createTessellated(VkContext& ctx, const std::string& vertSrc,
                                   const std::string& tescSrc, const std::string& teseSrc,
                                   const std::string& fragSrc);
```

- [ ] **Step 3: Implement `createTessellated`**

In `VkShader.cpp`, implement it by mirroring `create()` (compile 4 stages, make 4 modules, build the descriptor set layout). The layout MUST match the lit layout (the same bindings used by `createStandardLitShader`'s shader — which is the NON-skinned lit layout: UBO@0 + samplers 1-8,10-13) but with `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` added to binding 0 (UBO) and binding 13 (height map) stage flags. The simplest correct approach: copy the non-skinned lit layout-building block from `create()`, then OR `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` into `bindings[0].stageFlags` and the binding-13 entry's `stageFlags`. Reference skeleton:
```cpp
ShaderHandle VkShaderStore::createTessellated(VkContext& ctx, const std::string& vertSrc,
                                              const std::string& tescSrc, const std::string& teseSrc,
                                              const std::string& fragSrc) {
    auto vspv  = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto tcspv = compileGlsl(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tescSrc);
    auto tespv = compileGlsl(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, teseSrc);
    auto fspv  = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || tcspv.empty() || tespv.empty() || fspv.empty()) {
        Log::error("VkShaderStore: tessellated shader compile failed");
        return kInvalidHandle;
    }
    VkShader s{};
    auto makeModule = [&](const std::vector<std::uint32_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &out));
        return out != VK_NULL_HANDLE;
    };
    if (!makeModule(vspv,  s.vertexModule))   return kInvalidHandle;
    if (!makeModule(tcspv, s.tescModule))     return kInvalidHandle;  // (cleanup on failure as create() does)
    if (!makeModule(tespv, s.teseModule))     return kInvalidHandle;
    if (!makeModule(fspv,  s.fragmentModule)) return kInvalidHandle;

    // --- descriptor set layout: copy the NON-skinned lit layout (UBO@0 +
    //     samplers 1-8,10-13) from create(), then add TESE to binding 0 + 13. ---
    // (Reproduce the exact bindings[] array used by the lit create(); set
    //  bindings[0].stageFlags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
    //  and the binding-13 entry's stageFlags |= ...TESSELLATION_EVALUATION_BIT.)
    // ... create setLayout + pipelineLayout exactly as create() does ...

    const ShaderHandle h = nextHandle_++;
    shaders_[h] = s;
    return h;
}
```
NOTE: read the existing `create()` layout block and reproduce it faithfully (binding count, sampler bindings 1-8 + 10-13, height map at 13). The ONLY differences are the 4 modules and the two added TESE stage flags. Match the cleanup-on-failure pattern of `create()`. Also update `destroy`/`destroyAll` (wherever shader modules are destroyed) to destroy `tescModule`/`teseModule` when non-null.

- [ ] **Step 4: Build**

Run: `cmake --build build-vk --target ironcore`
Expected: PASS (method unused so far).

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkShader.h engine/render/backends/vulkan/VkShader.cpp
git commit -m "M50b: VkShader tessellation modules + createTessellated (TESE-aware lit layout)"
```

---

## Task 3: Tessellation + wireframe pipeline variants

**Files:** Modify `engine/render/backends/vulkan/VkPipeline.h`, `engine/render/backends/vulkan/VkPipeline.cpp`.

- [ ] **Step 1: Make `createGraphicsPipelineImpl` tess- + wireframe-aware**

In `VkPipeline.cpp`, modify the pipeline assembly so that:
- If `sh.tescModule != VK_NULL_HANDLE`: use a 4-entry `stages[]` (vert, tesc, tese, frag), set `ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`, and attach a tessellation state:
  ```cpp
  VkPipelineTessellationStateCreateInfo ts{};
  ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  ts.patchControlPoints = 3;
  // ... info.pTessellationState = &ts;  (only when tessellated)
  ```
  Else: the existing 2-stage vert+frag path (`info.pTessellationState = nullptr`).
- Add a `bool wireframe` parameter; set `rs.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;`. (Keep `cullMode = VK_CULL_MODE_BACK_BIT` — wireframe still culls; that's fine and standard.)

Concrete stage-array handling:
```cpp
    VkPipelineShaderStageCreateInfo stages[4]{};
    uint32_t stageCount = 0;
    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stageCount].module = sh.vertexModule; stages[stageCount].pName = "main"; ++stageCount;
    if (sh.tescModule != VK_NULL_HANDLE) {
        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        stages[stageCount].module = sh.tescModule; stages[stageCount].pName = "main"; ++stageCount;
        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        stages[stageCount].module = sh.teseModule; stages[stageCount].pName = "main"; ++stageCount;
    }
    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stageCount].module = sh.fragmentModule; stages[stageCount].pName = "main"; ++stageCount;
    // ...
    info.stageCount = stageCount;
    info.pStages = stages;
    const bool tess = (sh.tescModule != VK_NULL_HANDLE);
    ia.topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.pTessellationState = tess ? &ts : nullptr;
    rs.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
```

- [ ] **Step 2: Cache keyed by `(shader, wireframe)`**

In `VkPipeline.h`, change the cache to key on both. Simplest: a small struct/pair. In `VkPipeline.cpp`, change `pipelineFor` to take `bool wireframe` and match both:
```cpp
::VkPipeline VkPipeline::pipelineFor(VkContext& ctx, VkSwapchain& swap, const VkShader& sh, bool wireframe) {
    for (const auto& e : pipelines_)
        if (e.shader == &sh && e.wireframe == wireframe) return e.pipeline;
    auto p = createGraphicsPipeline(ctx, swap, scenePass_, sh, wireframe);
    pipelines_.push_back({&sh, wireframe, p});
    return p;
}
```
Update the `pipelines_` member type accordingly (e.g. `struct Entry { const VkShader* shader; bool wireframe; ::VkPipeline pipeline; };  std::vector<Entry> pipelines_;`). Thread `wireframe` through `createGraphicsPipeline` → `createGraphicsPipelineImpl`. Update any other `pipelineFor` callers to pass `wireframe=false` (preserving current behavior).

- [ ] **Step 3: Build**

Run: `cmake --build build-vk` (all targets — callers of `pipelineFor` must compile)
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkPipeline.h engine/render/backends/vulkan/VkPipeline.cpp
git commit -m "M50b: tessellation (patch-list, 4-stage) + wireframe pipeline variants"
```

---

## Task 4: Tessellation shader sources + Renderer creation API

**Files:** Modify `engine/render/StandardLitShader.h`, `engine/render/Renderer.h`, `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`, OpenGL + Mock renderers.

- [ ] **Step 1: Add the three tess shader sources to `StandardLitShader.h`**

`standardLitTessVertSource()` — passthrough (local space, NO mvp), outputs control-point attrs at loc 0-3:
```cpp
inline const char* standardLitTessVertSource() {
    return R"GLSL(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
layout(location = 0) out vec3 cpPos;
layout(location = 1) out vec3 cpNormal;
layout(location = 2) out vec2 cpUV;
layout(location = 3) out vec3 cpTangent;
void main() {
    cpPos = aPos; cpNormal = aNormal; cpUV = aUV; cpTangent = aTangent;
}
)GLSL";
}
```

`standardLitTescSource()` — fixed factor from `probeBoxMin.w` (UI), min 1:
```cpp
inline const char* standardLitTescSource() {
    return R"GLSL(#version 450
layout(vertices = 3) out;
layout(location = 0) in vec3 cpPos[];
layout(location = 1) in vec3 cpNormal[];
layout(location = 2) in vec2 cpUV[];
layout(location = 3) in vec3 cpTangent[];
layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp; mat4 model; mat4 lightViewProj;
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 emissive; vec4 cameraPos;
    vec4 materialParams; vec4 materialParams2; vec4 baseColorFactor;
    vec4 fogColor; vec4 lightCounts; vec4 pointPositions[16]; vec4 pointColors[16];
    mat4 reflectionViewProj; vec4 reflectionParams; vec4 clipPlane;
    vec4 probeBoxMin; vec4 probeBoxMax; vec4 probeCenter;
} u;
layout(location = 0) out vec3 tcPos[];
layout(location = 1) out vec3 tcNormal[];
layout(location = 2) out vec2 tcUV[];
layout(location = 3) out vec3 tcTangent[];
void main() {
    if (gl_InvocationID == 0) {
        float f = max(u.probeBoxMin.w, 1.0);  // M50b — UI tessellation factor
        gl_TessLevelInner[0] = f;
        gl_TessLevelOuter[0] = f;
        gl_TessLevelOuter[1] = f;
        gl_TessLevelOuter[2] = f;
    }
    tcPos[gl_InvocationID]     = cpPos[gl_InvocationID];
    tcNormal[gl_InvocationID]  = cpNormal[gl_InvocationID];
    tcUV[gl_InvocationID]      = cpUV[gl_InvocationID];
    tcTangent[gl_InvocationID] = cpTangent[gl_InvocationID];
}
)GLSL";
}
```

`standardLitTeseSource()` — barycentric interp + height displacement; emits lit varyings (loc 0-4):
```cpp
inline const char* standardLitTeseSource() {
    return R"GLSL(#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) in vec3 tcPos[];
layout(location = 1) in vec3 tcNormal[];
layout(location = 2) in vec2 tcUV[];
layout(location = 3) in vec3 tcTangent[];
layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp; mat4 model; mat4 lightViewProj;
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 emissive; vec4 cameraPos;
    vec4 materialParams; vec4 materialParams2; vec4 baseColorFactor;
    vec4 fogColor; vec4 lightCounts; vec4 pointPositions[16]; vec4 pointColors[16];
    mat4 reflectionViewProj; vec4 reflectionParams; vec4 clipPlane;
    vec4 probeBoxMin; vec4 probeBoxMax; vec4 probeCenter;
} u;
layout(set = 0, binding = 13) uniform sampler2D uHeightMap;  // M50a height (white=peak)
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;
void main() {
    vec3 bc = gl_TessCoord;
    vec3 localPos  = bc.x * tcPos[0]     + bc.y * tcPos[1]     + bc.z * tcPos[2];
    vec3 localNorm = normalize(bc.x * tcNormal[0]  + bc.y * tcNormal[1]  + bc.z * tcNormal[2]);
    vec2 uv        = bc.x * tcUV[0]      + bc.y * tcUV[1]      + bc.z * tcUV[2];
    vec3 localTan  = normalize(bc.x * tcTangent[0] + bc.y * tcTangent[1] + bc.z * tcTangent[2]);

    // Displace along the (local) normal by the height map. Sample at the same
    // scaled UV the fragment shader uses; displacement amount in reflectionParams.w.
    float h = textureLod(uHeightMap, uv * u.materialParams.x, 0.0).r;
    localPos += localNorm * (h * u.reflectionParams.w);

    vec4 world = u.model * vec4(localPos, 1.0);
    vWorldPos = world.xyz;
    vNormal   = mat3(u.model) * localNorm;
    vTangent  = mat3(u.model) * localTan;
    vUV       = uv;   // raw — the fragment shader applies materialParams.x itself
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(localPos, 1.0);
}
)GLSL";
}
```

- [ ] **Step 2: Add the tessellated-shader creation API**

In `Renderer.h`: declare a pure-virtual `createTessellatedShader(vert, tesc, tese, frag)` (mirroring `createShader`/`createSkinnedShader`), and a concrete convenience:
```cpp
    ShaderHandle createStandardTessellatedLitShader() {
        return createTessellatedShader(standardLitTessVertSource(), standardLitTescSource(),
                                       standardLitTeseSource(), standardLitFragSource());
    }
```
In `VulkanRenderer.{h,cpp}` implement `createTessellatedShader` → `shaders_.createTessellated(context_, v, tc, te, f)`. In the OpenGL renderer + MockRenderer, stub `createTessellatedShader` to return `kInvalidHandle` (tessellation is Vulkan-only; grep all `Renderer` subclasses so none is left abstract).

- [ ] **Step 3: Build engine + sandbox (glslang compiles tesc/tese)**

Run: `cmake --build build-vk` (all targets)
Expected: PASS — glslang compiles the tesc/tese sources; no abstract-subclass errors.

- [ ] **Step 4: Commit**

```bash
git add engine/render/StandardLitShader.h engine/render/Renderer.h engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp <opengl + mock renderer files>
git commit -m "M50b: tessellation shader sources + createTessellatedShader API"
```

---

## Task 5: Renderer per-draw integration (pipeline select + UBO params + wireframe)

**Files:** Modify `engine/render/Renderer.h`, `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`, OpenGL + Mock renderers.

- [ ] **Step 1: Add `setWireframe` + `setTessellationFactor` to the interface + stubs**

In `Renderer.h`:
```cpp
    // M50b — toggle global wireframe (line polygon mode) for scene geometry.
    virtual void setWireframe(bool enable) = 0;
    // M50b — fixed tessellation factor (subdivision level) for tessellated draws.
    virtual void setTessellationFactor(float factor) = 0;
```
VulkanRenderer.h: `void setWireframe(bool e) override { pendingWireframe_ = e; }` and `void setTessellationFactor(float f) override { pendingTessFactor_ = f; }`, with members `bool pendingWireframe_ = false;` and `float pendingTessFactor_ = 16.0f;`. OpenGL/Mock: empty stubs (grep all subclasses).

- [ ] **Step 2: Select the wireframe pipeline variant in `recordSceneDraw` (and skinned)**

In `recordSceneDraw`, change the pipeline fetch to pass `pendingWireframe_`:
```cpp
    ::VkPipeline pipe = pipelines_.pipelineFor(context_, swapchain_, sh, pendingWireframe_);
```
Do the same wherever `recordSkinnedDraw` fetches its pipeline (so wireframe applies to skinned meshes too).

- [ ] **Step 3: Set tess UBO params for tessellated draws**

In `recordSceneDraw`, after the `LitUbo ubo{}` is otherwise populated, detect a tessellated shader and set the tess params (and turn POM off):
```cpp
    const VkShader& sh = shaders_.get(call.shader);
    const bool tessellated = (sh.tescModule != VK_NULL_HANDLE);
    if (tessellated) {
        ubo.reflectionParams.w = call.material.heightScale;  // displacement amount (reuses M50a heightScale)
        ubo.probeBoxMin.w      = pendingTessFactor_;          // UI tessellation factor
        ubo.baseColorFactor.w  = 0.0f;                        // POM OFF (displacement is real geometry)
    }
```
(Place this AFTER the existing `baseColorFactor.w = pomScale` line so it overrides for tess draws. Confirm `sh` is fetched before this; the pipeline fetch already gets `sh`.)

- [ ] **Step 4: Build all + tests**

Run: `cmake --build build-vk && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: PASS (all existing tests; no abstract-subclass break).

- [ ] **Step 5: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp <opengl + mock files>
git commit -m "M50b: per-draw tess pipeline selection + displacement/factor UBO params + wireframe select"
```

---

## Task 6: 3-way demo + F2 wireframe toggle + tess-factor slider (visual gate)

**Files:** Modify `games/11-sandbox/main.cpp`.

- [ ] **Step 1: Create the tessellated shader + a third demo quad**

In `main.cpp`:
1. After `litShader`, create the tess shader:
```cpp
    const iron::ShaderHandle tessShader = renderer.createStandardTessellatedLitShader();
    if (tessShader == iron::kInvalidHandle)
        iron::Log::warn("sandbox: tessellated shader unavailable; skipping the tessellation demo quad");
```
2. Add a THIRD quad next to the M50a flat + POM quads (same stone albedo + height map, same `uvScale`, grazing angle). Its DrawCall uses `shader = tessShader`, `material.heightScale = 0.08f` (drives displacement for tess draws), and is only submitted if `tessShader` is valid. Label it "Tessellation" via the existing label path.
   (If the tess quad needs the same mesh as the others, reuse the existing quad mesh handle — patch topology is set by the pipeline, the mesh is the same triangle data.)

- [ ] **Step 2: F2 wireframe toggle + tess-factor slider**

Add a `bool wireframe = false;` near the camera state, and in the F-key handling block:
```cpp
        if (input.keyPressed(GLFW_KEY_F2)) {
            wireframe = !wireframe;
            renderer.setWireframe(wireframe);
        }
```
Add an ImGui slider in an editor/debug panel (match the existing panel style):
```cpp
        static float tessFactor = 16.0f;
        if (ImGui::SliderFloat("Tessellation factor", &tessFactor, 1.0f, 64.0f))
            renderer.setTessellationFactor(tessFactor);
```
Call `renderer.setTessellationFactor(tessFactor);` once at startup too (so the initial value is applied).

- [ ] **Step 3: Build + run the visual gate**

Run: `cmake --build build-vk --target sandbox` then run `build-vk\games\11-sandbox\Debug\sandbox.exe`.
Expected (visual gate — confirm with user):
- A third "Tessellation" quad shows real geometric relief (the stones bulge as actual geometry), unlike the POM quad.
- **F2** toggles wireframe: the tessellated quad shows a dense subdivided + displaced mesh; the flat/POM quads show their base triangles only (flat). The rest of the scene also renders wireframe.
- The **tessellation-factor slider** visibly changes subdivision density on the tess quad.
- At a grazing angle the tessellated quad's silhouette/edge bumps (real geometry) where POM's stays a flat outline — the payoff.
- No cracks on the tess quad; other materials unchanged.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M50b: 3-way tessellation demo quad + F2 wireframe + tess-factor slider (visual gate)"
```

---

## Self-review notes (spec coverage)

- Device features (tessellationShader + fillModeNonSolid) → Task 1. tesc/tese compilation → Task 1. VkShader tess modules + TESE-aware layout → Task 2. 4-stage patch-list pipeline + wireframe line variant → Task 3. Tess shader set reusing lit frag + displacement + lit varyings → Task 4. setWireframe/setTessellationFactor + per-draw selection + displacement/POM-off → Task 5. 3-way demo + F2 + slider → Task 6. Reuse-lit-frag, triangle patches, fixed factor, spare-slot params → Tasks 4/5. All spec sections covered.
- Out-of-scope (adaptive LOD, quad patches, arbitrary-mesh displacement) appear in no task — correct.

## Risks / verification reminders

- **Descriptor layout TESE flags:** the tess shader's UBO (binding 0) + height map (binding 13) must include `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT`, or the `tese` can't read them (validation error). Task 2.
- **Winding:** `tese` declares `ccw`; the pipeline culls back faces (`CULL_MODE_BACK`). If the tessellated quad renders inside-out (culled), flip to `cw` in the tese OR confirm the patch winding. Catch at the visual gate.
- **UBO layout in tesc/tese:** the LitUbo block in the tesc/tese MUST match the full C++ `LitUbo` (1008 bytes) field-for-field (it's duplicated in those sources) — copy it verbatim from the lit vert. A mismatch silently corrupts `probeBoxMin.w`/`reflectionParams.w`/`mvp`.
- **`recordSkinnedDraw`:** apply the wireframe pipeline-select there too (Task 5 Step 2). Skinned meshes aren't tessellated (no tess skinned shader), but they should still honor wireframe.
- **All `Renderer` subclasses:** `createTessellatedShader` + `setWireframe` + `setTessellationFactor` are new pure-virtuals — stub on OpenGL + Mock or the build breaks ([[verify-clean-build-before-ci]]). Build ALL targets.
- **Clean build before CI:** `cmake --build build-vk` (all) + `ctest` at the end.
- **Visual gate owns correctness:** tessellation/displacement/wireframe can't be unit-tested headless; the look + no-cracks + silhouette win are confirmed by running the sandbox.
