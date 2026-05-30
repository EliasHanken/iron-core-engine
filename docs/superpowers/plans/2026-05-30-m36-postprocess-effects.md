# M36 — Post-process Effects Core + Selection Effects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable Vulkan post-process framework (scene→offscreen color + tagged mask texture → full-screen composite chain) and ship three selection effects on it: silhouette outline, glowing outline, and x-ray.

**Architecture:** The scene renders into an offscreen color target instead of the swapchain. Tagged draw calls (`DrawCall::effectId != 0`) also render into a small mask texture. A post-process chain runs full-screen passes (edge-detect, separable blur, depth-aware tint) reading color+mask+depth, ping-ponging between two scratch targets, then composites to the swapchain. UI/overlays (ImGui, HUD, gizmo/debug lines) move to a stage drawn *after* composite so effects never bleed onto editor chrome. A backend-agnostic pure-logic core (`EffectStyle` table + pass planner + ping-pong math) is unit-tested; the GPU passes are user-verified.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), Vulkan 1.3, VMA, GLSL→SPIR-V, Dear ImGui (editor host only). Reference spec: `docs/superpowers/specs/2026-05-30-m36-postprocess-effects-design.md`.

**Verification model:** GPU/interactive passes are not reachable by the `ironcore`-linked unit harness (same as gizmo/shadows/reflections). So:
- **Pure logic** (effect-style table, pass-ordering, ping-pong index math) lives in backend-agnostic files with **no `vulkan.h` include** and is fully unit-tested.
- Every task's mechanical gate: **builds clean** (`cmake --build build-vk -j`) and the **existing 46 tests + new tests stay green** (`ctest --test-dir build-vk -C Debug`).
- **Final acceptance is user visual verification** (Phase E).
- **Vulkan boilerplate tasks name `VkReflectionTarget`/`VkShadowMap`/`VkSkybox` as the concrete templates to mirror.** Those existing files implement offscreen images, render passes, framebuffers, samplers, descriptor sets, and full-pipeline creation in this codebase's exact style. Follow them; do not invent new patterns.

**Build & test commands (used by every task):**
```bash
cmake --build build-vk -j
ctest --test-dir build-vk -C Debug --output-on-failure
```
(If `build-vk` is missing: `cmake -S . -B build-vk` first. A benign "LF will be replaced by CRLF" git warning on commit is expected on Windows. ImGui/GLFW `LNK4217`/`LNK4286` linker warnings are pre-existing and benign.)

---

## File Structure

**New (backend-agnostic, unit-tested — no `vulkan.h`):**
- `engine/render/PostEffect.h` — `EffectKind { None, Outline, GlowOutline, XRay }`, `EffectStyle` struct, `EffectTable` (256-entry style table with set/get/active tracking).
- `engine/render/PostChainPlan.h` / `.cpp` — pure planner: from the set of active effect kinds, produce the ordered list of post passes; plus ping-pong source/dest index math.
- `tests/test_postprocess_chain.cpp` — tests for `EffectTable` + `PostChainPlan`.

**New (Vulkan backend — GPU):**
- `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp` — owns offscreen scene-color, mask (id+depth), and two ping-pong scratch targets; the post render pass; full-screen pipelines (outline, glow-blur-H, glow-blur-V, glow-composite, xray, copy); and `runChain()`. Recreated on resize. Mirrors `VkReflectionTarget`.

**Modified (engine):**
- `engine/render/Renderer.h` — add `uint8_t effectId = 0` to `DrawCall`; add `EffectStyle`/`setEffectStyle` include + base no-op `virtual void setEffectStyle(uint8_t, const EffectStyle&)`.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — own a `VkPostProcess`; render scene into its offscreen color; record the mask pass for tagged calls; run the chain; composite to swapchain; move debug-lines/HUD/deferred-UI emission to after composite; recreate targets on `setViewport`; implement `setEffectStyle`.
- `engine/render/backends/opengl/OpenGLRenderer.*` — inherits the base no-op (compile-only; `effectId` ignored). No behavior change; verify it still builds.

**Modified (game/docs):**
- `games/11-sandbox/main.cpp` — `setEffectStyle(1, …)` at startup; set `effectId = 1` on the selected entity's draw call; an Inspector combo to pick the kind.
- `engine/editor/SceneInspector.h` / `.cpp` — add an effect-kind picker to the panel (returns chosen kind to host).
- `docs/engine/renderer.md` (created if absent) — document the post chain, `effectId`/`setEffectStyle`, new frame order.
- `docs/engine/editor.md` — note the selection silhouette/glow/x-ray.

**Phases** (each ends green + committable; Phase A is independently visually verifiable):
- **A** — Frame restructure: scene→offscreen→composite→swapchain, UI on top. No effects yet; scene looks identical.
- **B** — Pure-logic core (`PostEffect` + `PostChainPlan`) + tests.
- **C** — Mask pass + `effectId`/`setEffectStyle` + **outline** effect.
- **D** — **Glow** effect (separable blur, ping-pong).
- **E** — **X-ray** effect + sandbox wiring + Inspector picker + docs + visual verification.

---

## Phase A — Frame restructure (scene → offscreen → composite)

Goal: route the scene pass through an offscreen color target and composite it to the swapchain with a full-screen copy, and move UI/overlays to draw after the composite. After this phase the app looks **identical** but flows through the new pipeline — this de-risks the hardest part (render-pass surgery) before any effect exists.

### Task A1: Offscreen scene-color target + full-screen copy pipeline (`VkPostProcess` skeleton)

**Files:**
- Create: `engine/render/backends/vulkan/VkPostProcess.h`
- Create: `engine/render/backends/vulkan/VkPostProcess.cpp`
- Modify: `engine/render/backends/vulkan/CMakeLists.txt` (or the engine CMake list that globs/lists Vk sources — confirm by reading it)

**Template to mirror:** `engine/render/backends/vulkan/VkReflectionTarget.{h,cpp}` (offscreen color+depth image, render pass with exit dependency for sampling, framebuffer, shared sampler, begin/end pass helpers) and `VkSkybox.cpp` (full-screen-ish pipeline creation, descriptor set with a sampled image).

- [ ] **Step 1: Read the templates and the engine Vk CMake list**

Read `VkReflectionTarget.h`, `VkReflectionTarget.cpp`, `VkSkybox.cpp` (pipeline + descriptor creation), and the CMake file listing Vulkan backend sources. Confirm how sources are added (glob vs explicit) so the new files compile.

- [ ] **Step 2: Declare `VkPostProcess` with the scene-color target only (this task)**

Create `engine/render/backends/vulkan/VkPostProcess.h`:

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkPostProcess is Vulkan-only."
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace iron {

class VkContext;

// Owns the offscreen render targets and full-screen passes for the M36
// post-process chain. Phase A: a single offscreen scene-color target (+ depth)
// matching the swapchain, and a "copy" pipeline that blits it to the swapchain
// image. Later phases add the mask target, ping-pong scratch targets, and the
// effect pipelines. Recreated on resize. Mirrors VkReflectionTarget.
class VkPostProcess {
public:
    bool init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
              VkExtent2D extent, VkSampler sharedSampler,
              VkRenderPass swapchainPass);
    void destroy(VkContext& ctx);

    // Recreate size-dependent targets after a swapchain resize.
    bool resize(VkContext& ctx, VkExtent2D extent);

    // The render pass + framebuffer the SCENE should render into (offscreen).
    VkRenderPass  scenePass()        const { return scenePass_; }
    VkFramebuffer sceneFramebuffer() const { return sceneFb_; }
    VkExtent2D    extent()           const { return extent_; }

    // Begin/end the offscreen scene pass (issues viewport/scissor).
    void beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endScenePass(VkCommandBuffer cb) const;

    // Phase A composite: full-screen copy of scene-color into the currently
    // bound swapchain render pass. Caller has already begun the swapchain pass.
    void recordComposite(VkCommandBuffer cb) const;

private:
    VkContext* ctx_ = nullptr;           // not owned
    VkSampler  sampler_ = VK_NULL_HANDLE; // shared, not owned
    VkExtent2D extent_{};
    VkFormat   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat   depthFormat_ = VK_FORMAT_UNDEFINED;

    // Offscreen scene-color (+depth) target.
    VkImage       sceneColor_      = VK_NULL_HANDLE;
    VmaAllocation sceneColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneColorView_  = VK_NULL_HANDLE;
    VkImage       sceneDepth_      = VK_NULL_HANDLE;
    VmaAllocation sceneDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  scenePass_       = VK_NULL_HANDLE;
    VkFramebuffer sceneFb_         = VK_NULL_HANDLE;

    // Full-screen "copy" pipeline (samples sceneColor_, writes swapchain).
    VkDescriptorSetLayout copySetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      copyPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          copyPipeline_   = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_       = VK_NULL_HANDLE;
    VkDescriptorSet       copyDescSet_    = VK_NULL_HANDLE;

    bool createTargets(VkContext& ctx);
    void destroyTargets(VkContext& ctx);
    bool createCopyPipeline(VkContext& ctx, VkRenderPass swapchainPass);
};

}  // namespace iron
```

- [ ] **Step 3: Implement `VkPostProcess.cpp` scene-color target + copy pipeline**

Implement, mirroring `VkReflectionTarget.cpp` for the image/renderpass/framebuffer creation and `VkSkybox.cpp` for the descriptor-set + pipeline creation. Specifics:
- **Scene-color image:** `colorFormat` (the swapchain color format passed in), usage `COLOR_ATTACHMENT | SAMPLED`, VMA `GPU_ONLY`. View, plus a depth image in `depthFormat` (`D32_SFLOAT`) usage `DEPTH_STENCIL_ATTACHMENT`.
- **`scenePass_`:** 2 attachments (color + depth) exactly like `VkReflectionTarget`'s render pass, with an exit subpass dependency making the color readable as a sampled image (final layout `SHADER_READ_ONLY_OPTIMAL`).
- **Copy pipeline:** full-screen triangle (no vertex buffer — `gl_VertexIndex` trick), one combined-image-sampler binding (the scene color), no depth test, writes into the **swapchain** render pass (`swapchainPass` passed to `init`). Shaders: `kFullscreenVert` + `kCopyFrag` (added in Step 4).
- **`recordComposite`:** bind the copy pipeline + descriptor set, `vkCmdDraw(cb, 3, 1, 0, 0)`.
- **`resize`:** `destroyTargets` + `createTargets` + rewrite the copy descriptor set to the new scene-color view.

Use the file `engine/render/backends/vulkan/VkUtils.h` helpers if present for image/view creation (read it first).

- [ ] **Step 4: Add the full-screen vertex shader + copy fragment shader**

Post-process shaders are GLSL compiled at runtime like the renderer's other inline shaders. Add these as `constexpr const char*` in `VkPostProcess.cpp` (matching how `VkSkybox.cpp` embeds its shaders):

```glsl
// kFullscreenVert — emits a single screen-covering triangle; vUV in [0,1].
#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
```

```glsl
// kCopyFrag — blit the scene color straight through.
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
void main() { outColor = texture(uScene, vUV); }
```

- [ ] **Step 5: Build**

Run: `cmake --build build-vk -j`
Expected: builds clean (VkPostProcess compiles; not yet used).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/CMakeLists.txt
git commit -m "M36: VkPostProcess skeleton — offscreen scene-color target + copy pipeline"
```

### Task A2: Route the scene pass through the offscreen target

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (add `VkPostProcess postProcess_;` member + include)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (`init`, `endFrame`, `setViewport`)

**Context:** `endFrame()` currently begins the swapchain render pass and records, in order: scene draws, skinned draws, skybox, deferred callbacks (particles), debug lines, HUD — then ends the pass. (Read `endFrame` first; see plan spec for the structure.) We split this into: (1) offscreen scene pass with the geometry; (2) swapchain pass with composite + UI/overlays.

- [ ] **Step 1: Initialize `postProcess_` in `VulkanRenderer::init`**

After the swapchain + shared sampler exist (find where `reflection_.init(...)` is called and mirror its placement), add:

```cpp
if (!postProcess_.init(context_, swapchain_.colorFormat(), swapchain_.depthFormat(),
                       swapchain_.extent(), /*shared sampler*/ sharedSampler_,
                       swapchain_.renderPass())) {
    Log::error("VulkanRenderer: post-process init failed");
    return false;
}
```
(Use the same shared sampler the reflection target uses — read how `reflection_.init` gets its sampler and reuse it. If there's no member sampler, follow `VkReflectionTarget`'s sampler approach.)

- [ ] **Step 2: Split `endFrame` — geometry into offscreen, composite+UI into swapchain**

Replace the single scene-pass block in `endFrame` with two passes. The geometry (scene draws, skinned, skybox, deferred/particles) goes into `postProcess_.scenePass()`; the composite + debug-lines + HUD go into the swapchain pass:

```cpp
    // --- offscreen scene pass: geometry renders into the post-process color ---
    const float clear[4] = {pendingClear_.x, pendingClear_.y, pendingClear_.z, 1.0f};
    postProcess_.beginScenePass(cb, clear);
    {
        // replay buffered scene draws
        for (const auto& call : sceneDraws_)
            if (meshes_.valid(call.mesh)) recordSceneDraw(cb, call);
        for (std::size_t i = 0; i < skinnedDraws_.size(); ++i)
            recordSkinnedDraw(cb, skinnedDraws_[i], skinnedBoneMatricesStash_[i]);
        if (pendingSkybox_ != kInvalidHandle)
            skybox_.record(cb, pendingSkybox_, pendingView_, pendingProjection_);
        for (auto& fn : deferredScenePass_) fn(cb);   // particles
    }
    postProcess_.endScenePass(cb);

    // --- swapchain pass: composite the scene, then UI/overlays on top ---
    VkClearValue clears[2];
    clears[0].color = {{0, 0, 0, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = swapchain_.renderPass();
    rp.framebuffer = swapchain_.framebuffer(currentImageIndex_);
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain_.extent();
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0, 0, (float)swapchain_.extent().width,
                  (float)swapchain_.extent().height, 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scis{{0, 0}, swapchain_.extent()};
    vkCmdSetScissor(cb, 0, 1, &scis);

    postProcess_.recordComposite(cb);   // Phase A: straight copy

    if (pendingDebugFlush_)
        debugLines_.record(cb, pendingDebugView_, pendingDebugProj_);
    if (pendingHudValid_)
        hud_.record(cb, pendingHudBatch_, pendingHudW_, pendingHudH_);

    vkCmdEndRenderPass(cb);
```

> IMPORTANT: ImGui records via a deferred callback in the sandbox host (`enqueueDeferredScenePass`). For Phase A, ImGui must end up drawn on the **swapchain** pass, not the offscreen pass, or the editor UI gets post-processed later. Two options — pick the one matching how the sandbox enqueues ImGui (read `games/11-sandbox/main.cpp` + `engine/editor/ImGuiLayer.cpp`):
> - **(preferred)** Keep `deferredScenePass_` for particles (offscreen), and record ImGui/HUD/debug in the swapchain pass. If ImGui currently uses `enqueueDeferredScenePass`, add a second queue `enqueueDeferredUiPass(...)` that fires in the swapchain pass, and switch `ImGuiLayer` to it.
> - If ImGui is recorded directly by `ImGuiLayer` against `scenePass()`, update `ImGuiLayer` to target the swapchain pass instead.
>
> Implement whichever fits; the goal: **ImGui + HUD + gizmo/debug lines render in the swapchain pass, after composite.** Document the choice in the commit message.

- [ ] **Step 3: Handle resize in `setViewport`**

Find `setViewport` / `recreateSwapchainAndFramebuffers` and, after the swapchain is recreated, call:
```cpp
postProcess_.resize(context_, swapchain_.extent());
```

- [ ] **Step 4: Destroy `postProcess_` on shutdown**

In the destructor / teardown where `reflection_.destroy(context_)` is called, add `postProcess_.destroy(context_);` (before context teardown).

- [ ] **Step 5: Build**

Run: `cmake --build build-vk -j`
Expected: clean build.

- [ ] **Step 6: Run tests (regression guard)**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: 46/46 pass.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/editor/ImGuiLayer.cpp engine/editor/ImGuiLayer.h
git commit -m "M36: route scene through offscreen target; composite + UI on swapchain"
```

> **VERIFICATION CHECKPOINT (controller):** Phase A is the riskiest. After A2, ask the user to run the sandbox and confirm the scene + UI look **identical to before** (no black screen, no double UI, gizmo/ImGui crisp, resize works). Do not start Phase C until Phase A renders correctly. (Phase B is pure logic and can proceed in parallel/either order.)

---

## Phase B — Pure-logic core (`PostEffect` + `PostChainPlan`) + tests

Backend-agnostic, fully unit-tested. No `vulkan.h`.

### Task B1: `EffectKind` + `EffectStyle` + `EffectTable`

**Files:**
- Create: `engine/render/PostEffect.h`
- Test: `tests/test_postprocess_chain.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test (EffectTable behavior)**

Create `tests/test_postprocess_chain.cpp`:

```cpp
#include "render/PostEffect.h"
#include "render/PostChainPlan.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_effect_table_defaults() {
    iron::EffectTable t;
    CHECK(t.style(0).kind == iron::EffectKind::None);  // id 0 is always None
    CHECK(t.activeKinds().empty());                    // nothing set yet
}

static void test_effect_table_set_get() {
    iron::EffectTable t;
    iron::EffectStyle s;
    s.kind = iron::EffectKind::Outline;
    s.color = iron::Vec3{1, 0, 0};
    s.width = 3.0f;
    t.setStyle(1, s);
    CHECK(t.style(1).kind == iron::EffectKind::Outline);
    CHECK(t.style(1).width == 3.0f);
}

static void test_effect_table_active_kinds() {
    iron::EffectTable t;
    iron::EffectStyle a; a.kind = iron::EffectKind::Outline;
    iron::EffectStyle b; b.kind = iron::EffectKind::XRay;
    t.setStyle(1, a);
    t.setStyle(2, b);
    // activeKinds = the DISTINCT kinds of ids that are non-None, no duplicates.
    auto ks = t.activeKinds();
    CHECK(ks.size() == 2);
    bool hasOutline = false, hasXray = false;
    for (auto k : ks) { if (k == iron::EffectKind::Outline) hasOutline = true;
                        if (k == iron::EffectKind::XRay) hasXray = true; }
    CHECK(hasOutline && hasXray);
}

static void test_effect_table_setting_none_clears() {
    iron::EffectTable t;
    iron::EffectStyle a; a.kind = iron::EffectKind::Outline;
    t.setStyle(1, a);
    iron::EffectStyle none; none.kind = iron::EffectKind::None;
    t.setStyle(1, none);
    CHECK(t.activeKinds().empty());
}

int main() {
    test_effect_table_defaults();
    test_effect_table_set_get();
    test_effect_table_active_kinds();
    test_effect_table_setting_none_clears();
    // PostChainPlan tests appended in Task B2.
    if (g_failures == 0) std::printf("All post-process tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the test, run it, verify it fails to compile/link**

Add to `tests/CMakeLists.txt` (mirror an existing `iron_add_test` line):
```cmake
iron_add_test(test_postprocess_chain test_postprocess_chain.cpp)
```
Run: `cmake -S . -B build-vk && cmake --build build-vk --target test_postprocess_chain`
Expected: FAIL — `render/PostEffect.h` not found.

- [ ] **Step 3: Implement `PostEffect.h`**

Create `engine/render/PostEffect.h`:

```cpp
#pragma once

#include "math/Vec.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iron {

// What a tagged object's screen-space effect looks like. Backend-agnostic.
enum class EffectKind : uint8_t { None = 0, Outline, GlowOutline, XRay };

struct EffectStyle {
    EffectKind kind      = EffectKind::None;
    Vec3       color     = {1.0f, 0.6f, 0.1f};  // selection orange
    float      width     = 2.0f;                 // outline thickness / blur radius (px)
    float      intensity = 1.0f;                 // glow strength / x-ray opacity
};

// A fixed 256-entry table mapping effect id -> style. Id 0 is reserved as
// "no effect" and always reports None. The renderer indexes this by
// DrawCall::effectId. Pure data; unit-testable without any GPU.
class EffectTable {
public:
    static constexpr int kMaxIds = 256;

    void setStyle(uint8_t id, const EffectStyle& s) {
        if (id == 0) return;                 // id 0 is always None
        styles_[id] = s;
    }
    const EffectStyle& style(uint8_t id) const { return styles_[id]; }

    // The distinct non-None kinds present in the table (deduplicated). Drives
    // which post passes the chain must run this frame.
    std::vector<EffectKind> activeKinds() const {
        std::array<bool, 4> seen{};          // index by EffectKind value
        for (int i = 1; i < kMaxIds; ++i)
            seen[static_cast<int>(styles_[i].kind)] = true;
        std::vector<EffectKind> out;
        for (int k = 1; k < 4; ++k)          // skip None(0)
            if (seen[k]) out.push_back(static_cast<EffectKind>(k));
        return out;
    }

private:
    std::array<EffectStyle, kMaxIds> styles_{};  // all default to None
};

}  // namespace iron
```

- [ ] **Step 4: Run the EffectTable tests**

Run: `cmake --build build-vk --target test_postprocess_chain && ctest --test-dir build-vk -C Debug -R test_postprocess_chain --output-on-failure`
Expected: PASS (PostChainPlan tests not added yet — `PostChainPlan.h` must exist as an empty-ish header to compile; create it in B2. To keep this step green, temporarily include only `PostEffect.h`).

> NOTE: Step 1 includes `PostChainPlan.h`. To keep B1 self-contained, create a minimal `engine/render/PostChainPlan.h` stub now with just the include guard + namespace, and flesh it out in B2. (No placeholder logic — just an empty namespace until B2.)

- [ ] **Step 5: Commit**

```bash
git add engine/render/PostEffect.h engine/render/PostChainPlan.h tests/test_postprocess_chain.cpp tests/CMakeLists.txt
git commit -m "M36: EffectKind/EffectStyle/EffectTable + tests (pure logic)"
```

### Task B2: `PostChainPlan` — pass ordering + ping-pong math

**Files:**
- Modify: `engine/render/PostChainPlan.h`
- Create: `engine/render/PostChainPlan.cpp`
- Modify: `tests/test_postprocess_chain.cpp` (append tests + call them in `main`)
- Modify: `engine/CMakeLists.txt` (add `PostChainPlan.cpp` to the `ironcore` sources — confirm by reading)

- [ ] **Step 1: Append failing tests**

Add to `tests/test_postprocess_chain.cpp` (and call them from `main` before the summary):

```cpp
static void test_plan_empty_is_just_copy() {
    // No active kinds -> the chain is a single Copy pass (scene -> swapchain).
    auto passes = iron::planPostChain({});
    CHECK(passes.size() == 1);
    CHECK(passes[0] == iron::PostPass::Copy);
}

static void test_plan_outline() {
    auto passes = iron::planPostChain({iron::EffectKind::Outline});
    // Outline = one edge-detect pass that also composites; ends reaching swapchain.
    CHECK(passes.size() == 1);
    CHECK(passes[0] == iron::PostPass::Outline);
}

static void test_plan_glow_is_multipass() {
    auto passes = iron::planPostChain({iron::EffectKind::GlowOutline});
    // Glow = blurH -> blurV -> composite (3 passes).
    CHECK(passes.size() == 3);
    CHECK(passes[0] == iron::PostPass::GlowBlurH);
    CHECK(passes[1] == iron::PostPass::GlowBlurV);
    CHECK(passes[2] == iron::PostPass::GlowComposite);
}

static void test_plan_xray() {
    auto passes = iron::planPostChain({iron::EffectKind::XRay});
    CHECK(passes.size() == 1);
    CHECK(passes[0] == iron::PostPass::XRay);
}

static void test_pingpong_alternates() {
    // Source target index alternates 0,1,0,1...; dest is the other.
    CHECK(iron::pingPongSource(0) == 0);
    CHECK(iron::pingPongSource(1) == 1);
    CHECK(iron::pingPongSource(2) == 0);
    CHECK(iron::pingPongDest(0) == 1);
    CHECK(iron::pingPongDest(1) == 0);
}
```

- [ ] **Step 2: Run, verify fail**

Run: `cmake --build build-vk --target test_postprocess_chain`
Expected: FAIL — `planPostChain` / `PostPass` undefined.

- [ ] **Step 3: Implement `PostChainPlan.h`**

Replace the stub `engine/render/PostChainPlan.h`:

```cpp
#pragma once

#include "render/PostEffect.h"

#include <vector>

namespace iron {

// The concrete full-screen passes the renderer knows how to run.
enum class PostPass : uint8_t {
    Copy,           // straight blit scene -> output (no active effects)
    Outline,        // edge-detect the mask, composite over scene
    GlowBlurH,      // horizontal blur of the mask coverage into scratch
    GlowBlurV,      // vertical blur (scratch -> scratch)
    GlowComposite,  // add blurred halo over scene
    XRay,           // depth-aware tint where mask is occluded
};

// Pure planner: given the distinct active effect kinds this frame, produce the
// ordered list of passes to run. Empty input -> a single Copy. Order is stable:
// XRay (under), then Outline, then Glow (over), matching how the spec layers
// them. (Single-effect cases are the v1 norm; multi-effect ordering is defined
// here so adding effects later stays predictable.)
std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds);

// Ping-pong scratch target index helpers for multi-pass effects.
inline int pingPongSource(int passIndex) { return passIndex % 2; }
inline int pingPongDest(int passIndex)   { return (passIndex + 1) % 2; }

}  // namespace iron
```

- [ ] **Step 4: Implement `PostChainPlan.cpp`**

Create `engine/render/PostChainPlan.cpp`:

```cpp
#include "render/PostChainPlan.h"

namespace iron {

std::vector<PostPass> planPostChain(const std::vector<EffectKind>& activeKinds) {
    if (activeKinds.empty()) return {PostPass::Copy};

    std::vector<PostPass> passes;
    bool xray = false, outline = false, glow = false;
    for (auto k : activeKinds) {
        if (k == EffectKind::XRay)        xray = true;
        else if (k == EffectKind::Outline)     outline = true;
        else if (k == EffectKind::GlowOutline) glow = true;
    }
    // Layer order: x-ray tint first (under), outline next, glow halo last (over).
    if (xray)    passes.push_back(PostPass::XRay);
    if (outline) passes.push_back(PostPass::Outline);
    if (glow) {
        passes.push_back(PostPass::GlowBlurH);
        passes.push_back(PostPass::GlowBlurV);
        passes.push_back(PostPass::GlowComposite);
    }
    return passes;
}

}  // namespace iron
```

- [ ] **Step 5: Add `PostChainPlan.cpp` to the `ironcore` library sources**

Read `engine/CMakeLists.txt`; add `render/PostChainPlan.cpp` to the `ironcore` source list (the same list that has `render/ReflectionPlane.cpp`). The header-only `PostEffect.h` needs no entry.

- [ ] **Step 6: Run tests, verify pass**

Run: `cmake -S . -B build-vk && cmake --build build-vk --target test_postprocess_chain && ctest --test-dir build-vk -C Debug -R test_postprocess_chain --output-on-failure`
Expected: PASS (all EffectTable + PostChainPlan tests).

- [ ] **Step 7: Full suite**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: 47/47 (46 existing + new).

- [ ] **Step 8: Commit**

```bash
git add engine/render/PostChainPlan.h engine/render/PostChainPlan.cpp engine/CMakeLists.txt tests/test_postprocess_chain.cpp
git commit -m "M36: PostChainPlan pass-ordering + ping-pong math + tests"
```

---

## Phase C — Mask pass + `effectId`/`setEffectStyle` + outline effect

### Task C1: API surface — `DrawCall::effectId` + `Renderer::setEffectStyle`

**Files:**
- Modify: `engine/render/Renderer.h`

- [ ] **Step 1: Add `effectId` to `DrawCall` and the `setEffectStyle` API**

In `engine/render/Renderer.h`, add the include near the top:
```cpp
#include "render/PostEffect.h"
```
Add the field to `DrawCall` (after `material`):
```cpp
struct DrawCall {
    MeshHandle    mesh    = kInvalidHandle;
    ShaderHandle  shader  = kInvalidHandle;
    Mat4          model   = Mat4::identity();
    Material      material{};
    uint8_t       effectId = 0;   // 0 = no post-process effect; else an EffectTable id
};
```
Add the virtual with a base no-op, near `setReflectionPlane` (so OpenGL inherits a no-op):
```cpp
    // Configure the post-process effect style for an id used by DrawCall::effectId.
    // Vulkan-only; the base implementation ignores it (OpenGL has no post chain).
    virtual void setEffectStyle(uint8_t effectId, const EffectStyle& style) {
        (void)effectId; (void)style;
    }
```

- [ ] **Step 2: Build (whole project — verifies OpenGL backend still compiles)**

Run: `cmake --build build-vk -j`
Expected: clean. `DrawCall` gains a field with a default, so all existing submitters still compile; OpenGL inherits the no-op.

- [ ] **Step 3: Commit**

```bash
git add engine/render/Renderer.h
git commit -m "M36: add DrawCall::effectId + Renderer::setEffectStyle (base no-op)"
```

### Task C2: Mask target + mask pass in `VkPostProcess` / `VulkanRenderer`

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp`

**Template:** `VkShadowMap.cpp` (a depth-only offscreen pass that re-renders scene geometry with a minimal shader) is the closest analog for the mask pass; `VkReflectionTarget` for the target.

- [ ] **Step 1: Add the mask target to `VkPostProcess`**

Add to `VkPostProcess` (header): a mask color image (format `VK_FORMAT_R8_UINT` for the id, plus the object's depth — store depth by adding a depth attachment to the mask pass so x-ray can compare; the mask render pass thus has a color (id) + depth attachment). Add accessors `maskPass()`, `maskFramebuffer()`, `beginMaskPass()/endMaskPass()`, and a `descriptorImageInfo()` for sampling the mask id + the mask depth in later passes. Mirror the scene-color target creation.

> Decision (locked to avoid ambiguity): **mask color = `R8_UINT` carrying `effectId`; mask depth = `D32_SFLOAT` carrying the tagged object's depth.** Outline/glow sample the id (and use the scene-color silhouette); x-ray samples mask-id + compares mask-depth vs scene-depth.

- [ ] **Step 2: Add a mask pipeline + mask shader**

Add a pipeline in `VkPostProcess` that draws scene meshes into the mask target writing a constant id (passed via push constant `uint id`). Mask shaders (embed in `VkPostProcess.cpp`):

```glsl
// kMaskVert — position only; standard MVP from a push constant block.
#version 450
layout(location = 0) in vec3 aPos;
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
void main() { gl_Position = pc.mvp * vec4(aPos, 1.0); }
```

```glsl
// kMaskFrag — write the effect id (R8_UINT).
#version 450
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
layout(location = 0) out uint outId;
void main() { outId = pc.id; }
```

Expose `recordMaskDraw(cb, mvp, id, vertexBuffer, indexBuffer, indexCount)` — or, simpler, have `VulkanRenderer` bind the mask pipeline and issue draws using the existing mesh buffers (read `recordSceneDraw`/`recordShadowDraw` for how meshes are bound; mirror `recordShadowDraw`, which already does a position-only re-draw with a push-constant matrix).

- [ ] **Step 3: Record the mask pass in `endFrame`**

In `endFrame`, after the offscreen scene pass and before the swapchain pass, add (only when any tagged draws exist):

```cpp
    // --- mask pass: tagged draws write their effectId into the mask target ---
    bool anyTagged = false;
    for (const auto& call : sceneDraws_) if (call.effectId != 0) { anyTagged = true; break; }
    if (anyTagged) {
        postProcess_.beginMaskPass(cb);
        for (const auto& call : sceneDraws_) {
            if (call.effectId != 0 && meshes_.valid(call.mesh))
                recordMaskDraw(cb, call);   // new helper: binds mask pipeline, pushes mvp+id
        }
        postProcess_.endMaskPass(cb);
    }
```
Add `recordMaskDraw(VkCommandBuffer, const DrawCall&)` to `VulkanRenderer` mirroring `recordShadowDraw` (compute MVP from `pendingView_*pendingProjection_*model`, push `{mvp, call.effectId}`, bind the mesh, `vkCmdDrawIndexed`).

- [ ] **Step 4: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean build, 47/47. (Mask is written but not yet sampled — no visible change.)

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M36: mask target + mask pass (tagged draws write effectId)"
```

### Task C3: Outline pass + chain runner + `setEffectStyle`

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp`

- [ ] **Step 1: Add the outline pipeline + shader**

Add an outline pipeline to `VkPostProcess` (samples scene-color binding 0 + mask-id binding 1; push constant for color/width/extent). Shader (embed):

```glsl
// kOutlineFrag — composite a crisp silhouette edge of the mask over the scene.
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) usampler2D uMask;
layout(push_constant) uniform Push {
    vec4  color;     // rgb = outline color, a unused
    vec2  texel;     // 1/width, 1/height
    float width;     // outline thickness in pixels
} pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    uint here = texture(uMask, vUV).r;
    // Edge = this pixel differs from a neighbor `width` px away in the mask.
    float edge = 0.0;
    for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0) continue;
        vec2 o = vec2(float(dx), float(dy)) * pc.texel * pc.width;
        uint n = texture(uMask, vUV + o).r;
        if (n != here) edge = 1.0;
    }
    outColor = vec4(mix(scene, pc.color.rgb, edge), 1.0);
}
```

> Note: `usampler2D` reads the `R8_UINT` mask. Outline draws an edge wherever the mask id changes (object boundary), giving the true silhouette. Background (id 0) vs object (id N) boundary = the outline.

- [ ] **Step 2: Add the chain runner + `setEffectStyle` storage**

Add `EffectTable effects_;` to `VulkanRenderer`; implement:
```cpp
void VulkanRenderer::setEffectStyle(uint8_t id, const EffectStyle& s) { effects_.setStyle(id, s); }
```
Add `VkPostProcess::runChain(cb, const std::vector<PostPass>& passes, const EffectTable& effects, VkExtent2D ext, /*swapchain target*/ ...)` that, for each pass, binds the right pipeline + descriptor set, sets push constants from the relevant `EffectStyle`, and draws the full-screen triangle. For Phase C only `Copy`, `Outline` exist; `GlowBlur*/GlowComposite/XRay` land in D/E (the runner switch handles the kinds present, others added later).

In `endFrame`, replace the Phase-A `recordComposite` line with:
```cpp
    const auto passes = planPostChain(effects_.activeKinds());
    postProcess_.runChain(cb, passes, effects_, swapchain_.extent());
```
The runner's final pass must write into the swapchain pass (already begun). For `Outline`, it samples scene-color + mask and writes the composited result directly to the swapchain. For `Copy` (no effects), it blits scene-color (Phase A behavior preserved).

- [ ] **Step 3: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean, 47/47.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M36: outline post pass + chain runner + setEffectStyle"
```

> **VERIFICATION CHECKPOINT (controller):** wire a temporary `effectId=1` + `setEffectStyle(1,{Outline})` in the sandbox (or do Phase E1 first) and have the user confirm the silhouette renders. Phase E does the real wiring; this checkpoint can fold into Phase E.

---

## Phase D — Glowing outline (separable blur, ping-pong)

### Task D1: Glow pipelines + shaders

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp`

- [ ] **Step 1: Add blur + glow-composite pipelines**

Add three pipelines: `GlowBlurH`, `GlowBlurV` (both sample one texture, write to a scratch target), `GlowComposite` (samples scene + blurred-mask, writes swapchain). Use the two ping-pong scratch color targets (add them to `VkPostProcess`, `colorFormat`, `SAMPLED|COLOR_ATTACHMENT`, recreated on resize). BlurH reads the mask (converted to coverage), BlurV reads BlurH's scratch, GlowComposite reads BlurV's scratch.

Shaders (embed):
```glsl
// kGlowBlurFrag — separable Gaussian-ish blur; `dir` picks H or V.
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uSrc;   // coverage (r) in [0,1]
layout(push_constant) uniform Push { vec2 texel; vec2 dir; float radius; } pc;
void main() {
    float sum = 0.0, wsum = 0.0;
    for (int i = -6; i <= 6; ++i) {
        float w = exp(-float(i*i) / 18.0);
        vec2 o = pc.dir * pc.texel * (float(i) * pc.radius / 6.0);
        sum  += texture(uSrc, vUV + o).r * w;
        wsum += w;
    }
    outColor = vec4(vec3(sum / wsum), 1.0);
}
```
```glsl
// kGlowCoverageFrag — turn the uint mask into a float coverage for blurring.
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) usampler2D uMask;
void main() { outColor = vec4(vec3(texture(uMask, vUV).r > 0u ? 1.0 : 0.0), 1.0); }
```
```glsl
// kGlowCompositeFrag — add the halo (blurred coverage minus solid interior).
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBlur;   // blurred coverage
layout(set = 0, binding = 2) usampler2D uMask;          // original mask
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float blur = texture(uBlur, vUV).r;
    float solid = texture(uMask, vUV).r > 0u ? 1.0 : 0.0;
    float halo = max(blur - solid, 0.0) * pc.intensity;   // ring outside the object
    outColor = vec4(scene + pc.color.rgb * halo, 1.0);
}
```

> The chain for glow is: coverage(mask→scratchA) is folded into BlurH (BlurH can read the mask directly via the coverage step, or run coverage first). To keep passes = {GlowBlurH, GlowBlurV, GlowComposite} as the plan/tests specify, have **GlowBlurH sample the mask and emit blurred coverage in one pass** (combine coverage+H-blur), **GlowBlurV** blur the scratch vertically, **GlowComposite** add the halo. Adjust `kGlowBlurFrag` binding for the H pass to a `usampler2D uMask` variant if needed — implement as two tiny frag variants (H-from-mask, V-from-scratch) rather than adding a 4th pass, preserving the 3-pass plan.

- [ ] **Step 2: Extend `runChain` for the glow passes**

Handle `GlowBlurH` (mask → scratch[0]), `GlowBlurV` (scratch[0] → scratch[1]) using `pingPongSource/Dest`, and `GlowComposite` (scene + scratch[1] → swapchain) in the runner switch. Push constants from the `GlowOutline` style (`color`, `intensity`, `width`→radius).

- [ ] **Step 3: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean, 47/47 (the glow ping-pong order is already covered by `test_plan_glow_is_multipass`).

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M36: glowing-outline effect (separable blur ping-pong + halo composite)"
```

---

## Phase E — X-ray + sandbox wiring + Inspector picker + docs + verification

### Task E1: X-ray pass

**Files:**
- Modify: `engine/render/backends/vulkan/VkPostProcess.h` / `.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (mask must NOT depth-test against scene for x-ray footprint)

- [ ] **Step 1: Make the mask capture the un-occluded footprint + object depth**

The mask pass already writes id with its own depth attachment. For x-ray we need to know, per pixel, the tagged object's depth even where the scene occludes it. The mask pass renders only tagged objects with its **own** depth buffer (already the case), so `maskDepth` holds the tagged object's nearest depth independent of the full scene. Expose `maskDepth` as a sampled image (`descriptorImageInfo` for depth). Also expose the **scene depth** (from the offscreen scene pass) as sampled — add a sampled view of `sceneDepth_`.

- [ ] **Step 2: Add the x-ray pipeline + shader**

```glsl
// kXrayFrag — tint where the tagged object is occluded by nearer scene geometry.
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) usampler2D uMask;          // id
layout(set = 0, binding = 2) uniform sampler2D uMaskDepth;  // tagged obj depth
layout(set = 0, binding = 3) uniform sampler2D uSceneDepth; // full scene depth
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    uint id = texture(uMask, vUV).r;
    if (id == 0u) { outColor = vec4(scene, 1.0); return; }
    float md = texture(uMaskDepth, vUV).r;   // object depth
    float sd = texture(uSceneDepth, vUV).r;  // nearest scene depth
    // Occluded when something is in front of the object (smaller depth).
    float occluded = (sd < md - 1e-4) ? 1.0 : 0.0;
    outColor = vec4(mix(scene, pc.color.rgb, occluded * pc.intensity), 1.0);
}
```

- [ ] **Step 3: Handle `XRay` in `runChain`**

Bind scene-color + mask-id + mask-depth + scene-depth; push the `XRay` style; draw full-screen to the swapchain (or to the running target if combined with other effects — per `planPostChain`, XRay runs first/under).

- [ ] **Step 4: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean, 47/47.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M36: x-ray effect (depth-aware see-through tint)"
```

### Task E2: Inspector effect-kind picker

**Files:**
- Modify: `engine/editor/SceneInspector.h` / `.cpp`

- [ ] **Step 1: Extend the Inspector signature to surface a chosen effect kind**

In `SceneInspector.h`, change `draw` to also take the current selection's effect kind by reference (so the host can apply it). It already takes `GizmoSpace&` (M35); add `EffectKind&`:
```cpp
#include "editor/Gizmo.h"   // GizmoSpace
#include "render/PostEffect.h"  // EffectKind

bool draw(SceneEntity& entity, GizmoSpace& space, EffectKind& effectKind);
```

- [ ] **Step 2: Add the picker UI in `SceneInspector.cpp`**

After the Gizmo Space toggle (M35), add a combo:
```cpp
    ImGui::SeparatorText("Selection Effect");
    const char* kinds[] = {"None", "Outline", "Glowing Outline", "X-Ray"};
    int ki = static_cast<int>(effectKind);
    if (ImGui::Combo("Effect", &ki, kinds, 4))
        effectKind = static_cast<EffectKind>(ki);
```
(Do not fold into the `changed` return — same convention as the space toggle in M35.)

- [ ] **Step 3: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean (sandbox call site updated in E3 — if the build breaks on the old call, do E3 in the same task before building).

- [ ] **Step 4: Commit**

```bash
git add engine/editor/SceneInspector.h engine/editor/SceneInspector.cpp
git commit -m "M36: Inspector selection-effect picker"
```

### Task E3: Sandbox host wiring

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Track the selected effect kind + register the style**

Near the gizmo/inspector state (after `iron::Gizmo gizmo;`), add:
```cpp
    iron::EffectKind selectionEffect = iron::EffectKind::Outline;  // default
```
At startup (after the renderer is ready), register id 1's style:
```cpp
    {
        iron::EffectStyle s;
        s.kind  = selectionEffect;
        s.color = iron::Vec3{1.0f, 0.6f, 0.1f};  // selection orange
        s.width = 2.0f;
        s.intensity = 1.0f;
        renderer.setEffectStyle(1, s);
    }
```

- [ ] **Step 2: Pass the effect kind through the Inspector + re-register on change**

Update the inspector call (M35 left it as `inspector.draw(entity, sp)`):
```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            iron::GizmoSpace sp = gizmo.space();
            iron::EffectKind ek = selectionEffect;
            inspector.draw(scene.entities[selectedIndex], sp, ek);
            gizmo.setSpace(sp);
            if (ek != selectionEffect) {
                selectionEffect = ek;
                iron::EffectStyle s;
                s.kind = selectionEffect;
                s.color = iron::Vec3{1.0f, 0.6f, 0.1f};
                s.width = 2.0f; s.intensity = 1.0f;
                renderer.setEffectStyle(1, s);
            }
        }
```

- [ ] **Step 3: Tag the selected entity's draw call**

In the submit loop (where `iron::DrawCall call;` is built per resolved entity — read the render block), set the effect id for the selected entity:
```cpp
            call.effectId = (re.entityIndex == selectedIndex) ? 1 : 0;
```

- [ ] **Step 4: Build + tests**

Run: `cmake --build build-vk -j && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: clean, 47/47.

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M36: sandbox wiring — selected entity gets the chosen post effect"
```

### Task E4: Docs

**Files:**
- Create/Modify: `docs/engine/renderer.md`
- Modify: `docs/engine/editor.md`

- [ ] **Step 1: Document the post-process chain**

If `docs/engine/renderer.md` doesn't exist, create it with a "Post-process effects (M36)" section; otherwise append. Cover: the new frame order (scene→offscreen→mask→post chain→swapchain→UI), `DrawCall::effectId`, `EffectStyle`/`setEffectStyle`, the three effects, and that it's Vulkan-only (OpenGL ignores `effectId`). Note the reusable framework enables future effects (hit-flash, vignette, bloom) as add-a-shader work.

- [ ] **Step 2: Note selection effects in `editor.md`**

Add a short subsection under the gizmo/selection docs: the selected object can show an Outline / Glowing Outline / X-Ray, chosen in the Inspector's *Selection Effect* combo; the M35 oriented box still draws as an overlay.

- [ ] **Step 3: Commit**

```bash
git add docs/engine/renderer.md docs/engine/editor.md
git commit -m "docs(M36): post-process chain + selection effects"
```

### Task E5: User visual verification (acceptance gate)

- [ ] **Step 1: Hand off for visual verification**

Ask the user to run the sandbox and confirm:
1. **Outline** (default): selected object shows a crisp silhouette hugging its true shape (helmet reads as a helmet, not a box). The M35 box still appears as overlay (expected).
2. **Glowing Outline** (Inspector → Selection Effect): soft colored halo; interior not washed out.
3. **X-Ray**: move the selected object behind a wall → it shows through, tinted.
4. **No UI bleed**: ImGui, gizmo handles, HUD stay crisp/unaffected by effects.
5. **No regressions**: shadows, reflections, fog, skybox, particles, skinned meshes still render correctly; resize works.
6. **Perf**: smooth at 1280×720.

- [ ] **Step 2: On approval — finish the branch**

REQUIRED SUB-SKILL: Use superpowers:finishing-a-development-branch (this work is on `feat/m35-gizmo-local-space`; decide with the user whether M36 merges with M35 or on its own branch/PR). Then update memory (`iron-core-engine-progress.md`, `iron-core-engine-roadmap.md`): M36 post-process core + outline/glow/x-ray; note the framework is reusable for future effects (hit-flash, vignette, toon, bloom).

---

## Self-Review (performed against the spec)

- **Spec coverage:**
  - Part 1 frame restructure (scene→offscreen→mask→post→swapchain, UI-on-top) → Phase A (A1/A2).
  - Part 2 tagging API (`effectId` on DrawCall, `setEffectStyle`, Vulkan-only no-op base, host wiring single-source-of-truth) → C1, C3, E3.
  - Part 3 effects: outline → C3; glow (separable blur + ping-pong) → D1; x-ray (depth-aware) → E1. One-kind-per-id honored (EffectKind is a scalar; planPostChain layers multiple ids' kinds but each id is one kind).
  - Part 4 testing: pure-logic core (table, plan, ping-pong) → B1/B2 with `test_postprocess_chain.cpp`; existing 46 stay green (every phase runs ctest); visual gate → E5.
  - File structure matches the spec's list (PostEffect.h, PostChainPlan.{h,cpp}, VkPostProcess.{h,cpp}, the modified renderer/sandbox/inspector/docs).
- **Type/signature consistency:** `EffectKind { None, Outline, GlowOutline, XRay }`, `EffectStyle{kind,color,width,intensity}`, `EffectTable::{setStyle,style,activeKinds}`, `PostPass{Copy,Outline,GlowBlurH,GlowBlurV,GlowComposite,XRay}`, `planPostChain(vector<EffectKind>)→vector<PostPass>`, `pingPongSource/Dest(int)`, `DrawCall::effectId (uint8_t)`, `Renderer::setEffectStyle(uint8_t,const EffectStyle&)`, `SceneInspector::draw(SceneEntity&,GizmoSpace&,EffectKind&)` — used identically across tasks.
- **Placeholder scan:** GLSL shaders, pure-logic C++, headers, tests, and sandbox/inspector wiring are complete code. The raw-Vulkan object-creation steps are specified by exact formats/attachments/bindings + named template files (`VkReflectionTarget`/`VkShadowMap`/`VkSkybox`) to mirror — this is a deliberate, documented choice (see Verification model), not a "fill-in-details" placeholder, because inventing literal Vulkan boilerplate up front would be incorrect against the codebase's real helpers; the implementer reads the templates.
- **Phasing:** each phase ends green and committable; Phase A is independently visually verifiable before any effect exists (de-risks the render-pass surgery). Phase B is pure logic, order-independent of A.
- **Known follow-ups (not gaps):** one-kind-stacking (bitmask), saving effect to scene file, OpenGL support, scene-wide bloom/tonemapping — all explicitly out of scope per spec.
