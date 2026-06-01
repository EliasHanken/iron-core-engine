# M43a — Renderer Foundation: Composite to a Sampleable Viewport Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redirect the post-process composite **and** the in-swapchain overlays (debug lines, HUD) to render into a new offscreen **`viewportColor`** target, then blit that target to the swapchain — so the app looks **pixel-identical** today, but the scene now lives in a sampleable image with stable `view`+`sampler` accessors. This is the engine foundation M43b (docking + `ImGui::Image` viewport panel) builds on.

**Architecture:** `VkPostProcess` gains a `viewportColor` (color + depth) offscreen target whose render pass is **format-compatible** with the swapchain pass (same color + depth formats) so the existing composite / debug-line / HUD pipelines record into it **without rebuilding**. `VulkanRenderer::endFrame` runs the composite + overlays into the viewport pass (finalLayout → `SHADER_READ_ONLY`), then the swapchain pass becomes: blit `viewportColor` → swapchain, then ImGui (unchanged). The viewport target is sized independently of the swapchain via a new `resizeViewport(w,h)` (defaults to swapchain size, so behaviour is unchanged until M43b drives it from the Viewport panel).

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), Vulkan-only, VMA. Reference spec: `docs/superpowers/specs/2026-06-01-m43-docking-shell-design.md` (M43a implements its renderer half + the spec's "two-step de-risk" step 1).

**Branch:** already on `feat/m43-docking-shell` (spec commit `4f3579b` is on the branch). Every task commits here. (M43b will continue on the same branch or a follow-up; M43a merges first.)

**Build & test commands:**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
(Benign "LF→CRLF" git warning + pre-existing `LNK4217` warnings are expected.)

**Baseline:** 54 CTest cases as of M42 (`92d0c99`).

---

## Current state (read before starting)

Read these to ground every task:
- `engine/render/backends/vulkan/VkPostProcess.{h,cpp}` — owns the offscreen `sceneColor_`/`sceneDepth_` target + scene render pass (`createTargets`, lines ~474–624 of the .cpp), the copy/composite pipeline (`createCopyPipeline`), `recordComposite`, `runChain`/`runChainOffscreenPasses`, `resize`. **The viewportColor target mirrors the sceneColor target creation; the viewport-blit pipeline mirrors the copy pipeline.**
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — `endFrame` (lines ~817–1083). Today: shadow → reflection → scene pass (→`sceneColor_`) → mask → glow-offscreen → **swapchain pass { composite chain (`postProcess_.runChain`) → deferred UI (ImGui) → debug lines (`debugLines_.record`) → HUD (`hud_.record`) }** → submit/present. Note `setSceneViewport(cb, extent)` (negative-height Y-flip) is used for scene/debug; the composite uses a plain positive-height viewport.
- `engine/render/backends/vulkan/VulkanRenderer.h` — `postProcess_` member (line ~198); accessors `scenePass()`, `swapchainPass()`; private `recreateSwapchainAndFramebuffers`.
- `VulkanRenderer::init` (lines ~75 area): `postProcess_.init(context_, swapchain_.colorFormat(), swapchain_.depthFormat(), swapchain_.extent(), <sharedSampler>, swapchainPass())`. The shared sampler is `textures_`'s linear sampler (find the exact accessor used in the init call).
- Swapchain resize path: `recreateSwapchainAndFramebuffers` calls `postProcess_.resize(context_, swapchain_.extent())` (line ~441).
- `VkDebugLines::record(cb, device, VkFrameRing&, const Mat4& view, const Mat4& proj)` and `VkHud::record(cb, device, VkFrameRing&, VkTextureStore&, const HudBatch&, int w, int h)` — both pipelines built against `swapchainPass()` (lines ~85/89 of VulkanRenderer.cpp init).

**Key compatibility fact:** Vulkan pipelines are usable with any render pass that is *compatible* (same attachment count/formats/sample counts). The swapchain pass is color(`swapchain_.colorFormat()`) + depth(`swapchain_.depthFormat()`). If `viewportColor`'s render pass uses the **same two formats**, the composite/debug-line/HUD pipelines (built against `swapchainPass()`) record into the viewport pass with **no rebuild**.

---

## File structure

**Modified (engine):**
- `engine/render/backends/vulkan/VkPostProcess.h` — declare the viewport target (image/view/depth/pass/framebuffer), `beginViewportPass`/`endViewportPass`/`blitToSwapchain`, `viewportColorView()`/`viewportSampler()`, and grow `init`/`resize` to build/rebuild it. Add an independent `resizeViewport(ctx, extent)` OR fold into `resize` (see Task 1).
- `engine/render/backends/vulkan/VkPostProcess.cpp` — implement the above by mirroring the existing scene-target + copy-pipeline code.
- `engine/render/backends/vulkan/VulkanRenderer.h` — add `void resizeViewport(uint32_t w, uint32_t h);` (public, engine-internal) + `VkImageView viewportColorView() const;` + `VkSampler viewportSampler() const;` accessors; a `VkExtent2D viewportExtent_` member.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — restructure `endFrame` (composite + debug + HUD into the viewport pass; swapchain pass = blit + ImGui); wire `init` to size the viewport target to the swapchain initially; implement the accessors + `resizeViewport`.

**No test files** — M43a is a rendering-equivalence change (the scene must look identical). The gate is visual + a one-shot validation toggle (Task 4). CTest stays at 54.

**Untouched on purpose:** ImGui (M43b), the sandbox interaction code (M43b), all scene/mask/glow logic, shipping games.

---

## Phases

- **Phase A — viewportColor target + passes in VkPostProcess** (Tasks 1–2): create the target + render pass + framebuffer (mirroring scene target), and a `blitToSwapchain` copy pipeline (mirroring the existing copy pipeline). Resize support.
- **Phase B — endFrame redirect in VulkanRenderer** (Task 3): composite + debug + HUD into the viewport pass; swapchain pass = blit + ImGui; accessors + `resizeViewport`; init sizing.
- **Phase C — gate + PR + merge + memory** (Task 4).

Total: 4 tasks.

---

## Phase A — viewportColor target in VkPostProcess

### Task 1: Create the `viewportColor` color+depth target + render pass + framebuffer

**Files:** `engine/render/backends/vulkan/VkPostProcess.{h,cpp}`

This target holds the final composited scene + overlays, sampleable by the swapchain blit (and, in M43b, by ImGui). Its render pass must be **format-compatible** with the swapchain pass.

- [ ] **Step 1: Declare the target + a viewport extent in `VkPostProcess.h`**

In the private section (near the scene target fields, ~line 128), add:
```cpp
    // --- M43a: final composited "viewport" target (color + depth). Sized
    // independently of the swapchain (defaults to swapchain extent). The
    // composite + debug-line + HUD overlays render here; the swapchain pass
    // then blits this image (and, in M43b, ImGui samples it directly). ---
    VkExtent2D    viewportExtent_{};
    VkImage       viewportColor_      = VK_NULL_HANDLE;
    VmaAllocation viewportColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   viewportColorView_  = VK_NULL_HANDLE;
    VkImage       viewportDepth_      = VK_NULL_HANDLE;
    VmaAllocation viewportDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   viewportDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  viewportPass_       = VK_NULL_HANDLE;
    VkFramebuffer viewportFb_         = VK_NULL_HANDLE;
```
In the public section, add accessors + pass control:
```cpp
    VkImageView   viewportColorView() const { return viewportColorView_; }
    VkSampler     viewportSampler()   const { return sampler_; }
    VkExtent2D    viewportExtent()     const { return viewportExtent_; }

    // Begin/end the offscreen viewport pass (color cleared to clearColor,
    // depth cleared to 1.0). Composite + debug-lines + HUD record between these.
    void beginViewportPass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endViewportPass(VkCommandBuffer cb) const;

    // Resize ONLY the viewport target (scene/mask/glow targets unchanged).
    // Used by VulkanRenderer::resizeViewport. Recreates the image+view+fb.
    bool resizeViewport(VkContext& ctx, VkExtent2D extent);
```
Also declare two private helpers near `createTargets`:
```cpp
    bool createViewportTarget(VkContext& ctx);   // image+view+depth+pass+fb
    void destroyViewportTarget(VkContext& ctx);
```

- [ ] **Step 2: Implement `createViewportTarget` in `VkPostProcess.cpp`**

Mirror the scene-color + scene-depth image/view creation and the scene render pass in `createTargets` (lines ~474–624), with these deltas:
- Use `viewportExtent_` (NOT `extent_`) for image extents + framebuffer size.
- **Color image** `viewportColor_`/`viewportColorView_`: identical to `sceneColor_` (format `colorFormat_`, usage `COLOR_ATTACHMENT | SAMPLED`).
- **Depth image** `viewportDepth_`/`viewportDepthView_`: identical to `sceneDepth_` (format `depthFormat_`) but usage only `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` (no `SAMPLED` — nothing samples viewport depth).
- **Render pass** `viewportPass_`: 2 attachments matching the swapchain pass formats so it is pipeline-compatible:
  - color: format `colorFormat_`, `loadOp = CLEAR`, `storeOp = STORE`, `initialLayout = UNDEFINED`, **`finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`** (so the blit/ImGui can sample it).
  - depth: format `depthFormat_`, `loadOp = CLEAR`, `storeOp = DONT_CARE`, `initialLayout = UNDEFINED`, `finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`.
  - Subpass deps: reuse the scene pass's 2 color/depth deps pattern — entry `FRAGMENT_SHADER → COLOR_ATTACHMENT_OUTPUT|EARLY|LATE_FRAGMENT_TESTS` (so frame N+1 doesn't overwrite while N samples), and exit (color) `COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER` (so the blit samples safely). No depth-sample exit dep needed.
- **Framebuffer** `viewportFb_`: attachments `{viewportColorView_, viewportDepthView_}`, render pass `viewportPass_`, size `viewportExtent_`.

Call `createViewportTarget(ctx)` from `init` AFTER `createTargets` succeeds — but `viewportExtent_` must be set first. In `init`, after `extent_ = extent;` add `viewportExtent_ = extent;` (defaults the viewport target to the swapchain size). Add the `createViewportTarget` call to `init`'s sequence (after `createTargets`):
```cpp
    if (!createTargets(ctx))                       { destroy(ctx); return false; }
    if (!createViewportTarget(ctx))                { destroy(ctx); return false; }
```

- [ ] **Step 3: Implement `destroyViewportTarget`, `beginViewportPass`, `endViewportPass`**

`destroyViewportTarget` mirrors the scene-target teardown in `destroyTargets` (destroy framebuffer, render pass, both image views, both `vmaDestroyImage`), guarding each handle and nulling it. Call it from `destroy()` (add `destroyViewportTarget(ctx);` near `destroyTargets(ctx);`).

`beginViewportPass` mirrors `beginScenePass` but uses `viewportPass_`/`viewportFb_`/`viewportExtent_` and a **plain positive-height** viewport (the composite is UV-space; the debug/HUD overlays set their own viewport — see Task 3). `endViewportPass` is `vkCmdEndRenderPass(cb)`.

- [ ] **Step 4: Implement `resizeViewport`**

```cpp
bool VkPostProcess::resizeViewport(VkContext& ctx, VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return true;  // ignore degenerate
    if (extent.width == viewportExtent_.width &&
        extent.height == viewportExtent_.height) return true;  // no-op
    vkDeviceWaitIdle(ctx.device());
    destroyViewportTarget(ctx);
    viewportExtent_ = extent;
    return createViewportTarget(ctx);
}
```
Also: the existing `VkPostProcess::resize` (called on swapchain resize) must keep the viewport target valid. For M43a the viewport target tracks the swapchain, so at the end of `resize` (after the scene/mask/glow recreate), append: `if (!resizeViewport(ctx, extent)) return false;` **but** `resizeViewport` early-returns when the extent is unchanged — on a swapchain resize the extent DOES change, so it recreates. (M43b will decouple this; for M43a, viewport follows swapchain.) Guard against the `vkDeviceWaitIdle` double-wait by noting `resize` is already called outside frame recording.

- [ ] **Step 5: Build**
```bash
cmake --build build-vk --config Debug --target ironcore
```
Expected: clean compile. Nothing uses the new target yet — this task only adds it.

- [ ] **Step 6: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "M43a: VkPostProcess viewportColor target (color+depth, swapchain-compatible pass)"
```

---

### Task 2: `blitToSwapchain` copy pipeline (samples viewportColor → swapchain)

**Files:** `engine/render/backends/vulkan/VkPostProcess.{h,cpp}`

The swapchain pass will blit the composited viewport image to the backbuffer. Mirror the existing copy/composite pipeline (`createCopyPipeline` + `recordComposite`), but sampling `viewportColorView_`.

- [ ] **Step 1: Declare in `VkPostProcess.h`**

Private (near the copy pipeline fields):
```cpp
    // --- M43a: viewport→swapchain blit (own copy pipeline + descriptor set,
    // built against the swapchain pass, sampling viewportColorView_). ---
    VkDescriptorSetLayout blitSetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      blitPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          blitPipeline_   = VK_NULL_HANDLE;
    VkDescriptorSet       blitDescSet_    = VK_NULL_HANDLE;
```
Public:
```cpp
    // Full-screen blit of viewportColor into the (already-begun) swapchain pass.
    void blitToSwapchain(VkCommandBuffer cb) const;
```
Private helper: `bool createBlitPipeline(VkContext& ctx, VkRenderPass swapchainPass);`

- [ ] **Step 2: Implement `createBlitPipeline`**

Mirror `createCopyPipeline` exactly (same `kFullscreenVert` + `kCopyFrag` shaders — a single `sampler2D` at set 0 binding 0, full-screen triangle). Differences:
- Allocate `blitDescSet_` from the existing `descPool_` (it already has `COMBINED_IMAGE_SAMPLER` capacity for the copy/outline/glow/xray sets; if the pool size is tight, bump its `maxSets`/pool sizes by 1 set + 1 sampler — check the `descPool_` creation in `createCopyPipeline`/wherever the pool is made, and increase counts to cover one more set).
- Write `blitDescSet_` binding 0 = `{sampler_, viewportColorView_, SHADER_READ_ONLY_OPTIMAL}`.
- Build the graphics pipeline against `swapchainPass` (the arg), like the copy pipeline.

Call `createBlitPipeline(ctx, swapchainPass)` in `init` after `createCopyPipeline`:
```cpp
    if (!createCopyPipeline(ctx, swapchainPass))   { destroy(ctx); return false; }
    if (!createBlitPipeline(ctx, swapchainPass))   { destroy(ctx); return false; }
```

- [ ] **Step 3: Implement `blitToSwapchain` + descriptor rewrite on resize + teardown**

```cpp
void VkPostProcess::blitToSwapchain(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            blitPipeLayout_, 0, 1, &blitDescSet_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
```
In `resizeViewport` (Task 1 Step 4), after recreating the target, **re-write `blitDescSet_`** to point at the new `viewportColorView_` (mirror the copy-descriptor rewrite block in `VkPostProcess::resize`, lines ~256–270, but for `blitDescSet_` + `viewportColorView_`). In `destroy`, tear down `blitPipeline_`/`blitPipeLayout_`/`blitSetLayout_` (mirror the copy-pipeline teardown; `blitDescSet_` is freed with `descPool_`).

- [ ] **Step 4: Build**
```bash
cmake --build build-vk --config Debug --target ironcore
```
Expected: clean compile.

- [ ] **Step 5: Commit**
```bash
git add engine/render/backends/vulkan/VkPostProcess.h engine/render/backends/vulkan/VkPostProcess.cpp
git commit -m "M43a: VkPostProcess blitToSwapchain copy pipeline (samples viewportColor)"
```

---

## Phase B — endFrame redirect

### Task 3: Composite + overlays into the viewport pass; swapchain = blit + ImGui

**Files:** `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`

- [ ] **Step 1: Add accessors + member + `resizeViewport` decl in `VulkanRenderer.h`**

In the engine-internal accessors section:
```cpp
    // M43a: the final composited scene+overlays image, sampleable. M43b's
    // ImGui Viewport panel binds this via ImGui_ImplVulkan_AddTexture.
    VkImageView viewportColorView() const;
    VkSampler   viewportSampler() const;

    // M43a: resize the offscreen viewport target independently of the swapchain.
    // M43b drives this from the Viewport panel's content size. No-op on
    // unchanged/zero extent. Calls vkDeviceWaitIdle internally.
    void resizeViewport(uint32_t width, uint32_t height);
```

- [ ] **Step 2: Implement the accessors + `resizeViewport` in `VulkanRenderer.cpp`**

Near `scenePass()`:
```cpp
VkImageView VulkanRenderer::viewportColorView() const { return postProcess_.viewportColorView(); }
VkSampler   VulkanRenderer::viewportSampler()   const { return postProcess_.viewportSampler(); }

void VulkanRenderer::resizeViewport(uint32_t width, uint32_t height) {
    postProcess_.resizeViewport(context_, VkExtent2D{width, height});
}
```

- [ ] **Step 3: Restructure `endFrame`'s swapchain-pass block (lines ~983–1048)**

Today the block: begins the swapchain pass → composite (`runChain`) → deferred UI → restore scene viewport → debug lines → HUD → end pass. Replace it with: **(a)** an offscreen viewport pass running composite + debug lines + HUD, then **(b)** the swapchain pass running the blit + deferred UI (ImGui).

Replace the entire `// --- M36 Pass 4: swapchain pass ... ---` block with:
```cpp
    // --- M43a Pass 4: viewport pass — composite scene + overlays into the
    // offscreen sampleable target (instead of straight to the swapchain). ---
    {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        postProcess_.beginViewportPass(cb, clear);

        // Composite chain (Copy/Outline/GlowComposite/XRay) — UV-space, plain
        // positive-height viewport (beginViewportPass already set one).
        {
            const std::vector<PostPass> passes = planPostChain(activeKindsThisFrame);
            postProcess_.runChain(cb, passes, effects_, postProcess_.viewportExtent());
        }

        // Debug-line + HUD overlays render into the viewport target now (they
        // were previously in the swapchain pass). Debug lines use the
        // negative-height scene viewport; HUD sets its own.
        setSceneViewport(cb, postProcess_.viewportExtent());
        if (pendingDebugFlush_) {
            debugLines_.record(cb, context_.device(), frames_,
                               pendingDebugView_, pendingDebugProj_);
            pendingDebugFlush_ = false;
        }
        if (pendingHudValid_) {
            hud_.record(cb, context_.device(), frames_, textures_,
                        pendingHudBatch_, pendingHudW_, pendingHudH_);
            pendingHudValid_ = false;
        }

        postProcess_.endViewportPass(cb);
    }

    // --- M43a Pass 5: swapchain pass — blit viewport image, then UI on top. ---
    {
        VkClearValue clears[2]{};
        clears[0].color.float32[0] = 0.0f;
        clears[0].color.float32[1] = 0.0f;
        clears[0].color.float32[2] = 0.0f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = pipelines_.renderPass();
        rpBegin.framebuffer = pipelines_.framebuffer(currentImageIndex_);
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = swapchain_.extent();
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        // Plain positive-height viewport covering the swapchain (UV-space blit).
        {
            VkViewport vp{};
            vp.x = 0.0f; vp.y = 0.0f;
            vp.width  = static_cast<float>(swapchain_.extent().width);
            vp.height = static_cast<float>(swapchain_.extent().height);
            vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cb, 0, 1, &vp);
            VkRect2D scissor{{0, 0}, swapchain_.extent()};
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }

        // Blit the composited viewport image to the backbuffer.
        postProcess_.blitToSwapchain(cb);

        // UI/overlays (ImGui) on top — unchanged.
        for (auto& fn : deferredUiPass_) {
            fn(cb);
        }
        deferredUiPass_.clear();

        vkCmdEndRenderPass(cb);
    }
```
Notes for the implementer:
- The composite `runChain` previously took `swapchain_.extent()`; it now takes `postProcess_.viewportExtent()`. For M43a these are equal (viewport tracks swapchain), so output is identical. The chain's offscreen pre-passes (`runChainOffscreenPasses`, mask pass, glow) are UNCHANGED and still run before this block on `swapchain_.extent()` — leave them as-is (they operate on the scene/mask/glow targets which remain swapchain-sized; viewport-sized decoupling of those is M43b's concern and not needed while extents match).
- Debug lines + HUD moved OUT of the swapchain pass INTO the viewport pass. Confirm `debugLines_`/`hud_` pipelines are render-pass-compatible with `viewportPass_` (same color+depth formats — guaranteed by Task 1). They were built against `swapchainPass()`; compatibility makes this valid.
- ImGui stays in the swapchain pass (unchanged) — M43a does not touch ImGui.

- [ ] **Step 4: Ensure init sizes the viewport target (already defaulted in Task 1)**

`postProcess_.init` now creates the viewport target at `swapchain_.extent()` (Task 1 set `viewportExtent_ = extent`). No extra wiring needed. Confirm `recreateSwapchainAndFramebuffers` still calls `postProcess_.resize(...)` (which now also resizes the viewport target via Task 1 Step 4) — leave that call as-is.

- [ ] **Step 5: Build + run the full suite**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; 54/54 green (no test changes — this is a rendering-equivalence change).

- [ ] **Step 6: Commit**
```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M43a: endFrame composites + overlays into viewport target, blits to swapchain"
```

---

## Phase C — verification + PR + merge

### Task 4: Visual gate (rendering equivalence) + push + PR + squash-merge + memory

- [ ] **Step 1: Full clean build + tests**
```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 54/54 green.

- [ ] **Step 2: User visual gate**

Hand back to the user with this checklist (run `\build-vk\games\11-sandbox\Debug\sandbox.exe`). M43a is a **rendering-equivalence** change — the proof is "nothing looks different," because the scene is now routed through the viewport target + blit:

| Action | Expected |
|---|---|
| Open sandbox | Scene, panels, selection outline, gizmo all render exactly as before M43a. |
| Select / move entities (gizmo) | Selection outline + gizmo render correctly (they now render into the viewport target, then blit). |
| M42: add a collider | Green collider wireframe still shows (it's a `drawLineOverlay` overlay — now in the viewport target). |
| Enter Play (F5), M42 cube falls | Physics + Play banner render correctly. |
| Resize the OS window | Scene resizes correctly, no validation errors, no flicker (viewport target tracks the swapchain). |
| Selection effects (Inspector → Glowing Outline / X-Ray) | Effect renders correctly in the scene. |

If anything renders differently (missing outline/gizmo/HUD, wrong colors, validation errors on resize), the regression is in Task 3's pass restructuring or Task 1's render-pass compatibility — fix before proceeding.

- [ ] **Step 3: Push the branch**
```bash
git push -u origin feat/m43-docking-shell
```

- [ ] **Step 4: Open the PR**

Create `tmp/m43a-pr-body.md`:
```markdown
## Summary

M43a — renderer foundation for the editor docking shell (M43). The post-process
composite + debug-line/HUD overlays now render into a new offscreen, sampleable
`viewportColor` target, which is blitted to the swapchain. **The app looks
pixel-identical** — but the scene now lives in a sampleable image with stable
`viewportColorView()` / `viewportSampler()` accessors, and a `resizeViewport(w,h)`
that sizes it independently of the swapchain. This is the foundation M43b
(`ImGui::Image` Viewport panel + docking) builds on, and implements the spec's
two-step de-risk (step 1).

- `VkPostProcess`: new `viewportColor` (color+depth) target whose render pass is
  format-compatible with the swapchain pass, so the existing composite /
  debug-line / HUD pipelines record into it without rebuilding. New
  `beginViewportPass`/`endViewportPass`, a `blitToSwapchain` copy pipeline
  (mirrors the existing copy pipeline, sampling the viewport image), and
  `resizeViewport`.
- `VulkanRenderer::endFrame`: composite + debug lines + HUD run into the viewport
  pass; the swapchain pass blits the viewport image then draws ImGui (unchanged).
  New `viewportColorView()`/`viewportSampler()`/`resizeViewport()` accessors;
  the viewport target tracks the swapchain extent for now (M43b decouples it).

## Test plan
- [x] Full suite green (54/54) — rendering-equivalence change, no test changes
- [x] Visual: scene, selection outline, gizmo, collider wireframes, HUD, Play
      mode, post-effects, and window resize all render identically to pre-M43a.

## Known limits (intentional, this is a foundation)
- The viewport image is still blitted to the swapchain (not yet shown in an
  ImGui panel) — that's M43b.
- Viewport target tracks the swapchain size; independent sizing from a Viewport
  panel is M43b.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```
```bash
gh pr create --title "M43a: renderer foundation - composite to a sampleable viewport target" --body-file tmp/m43a-pr-body.md
```

- [ ] **Step 5: Watch CI**
```bash
gh pr checks --watch
```
Expected: `Build & test (Windows / MSVC)` passes (~6–8 min). Re-run transient flakes with `gh run rerun <id> --failed`.

- [ ] **Step 6: Squash-merge**
```bash
gh pr merge --squash --delete-branch
git checkout main && git reset --hard origin/main
git log --oneline -3
```
(`reset --hard origin/main` because the spec commit on this branch is folded into the squash — same situation as M42; nothing unique is lost.)

- [ ] **Step 7: Update memory**

- `MEMORY.md` index `iron-core-engine-progress` line: note M43a merged (PR #, SHA) — "viewport render target foundation; scene now in a sampleable image; M43b (docking + ImGui::Image viewport) next."
- `iron-core-engine-roadmap.md`: under the editor-overhaul arc, mark M43a done and note M43b is the immediate next step (docking branch + viewport panel + input rerouting).
- `iron-core-engine-progress.md`: append an `M43a` entry (merge SHA, one-paragraph summary, the rendering-equivalence approach + the viewport-target accessors, and that it's step 1 of the M43 docking shell).

---

## Acceptance criteria

1. `VkPostProcess` owns a `viewportColor` color+depth target with a render pass format-compatible with the swapchain pass; `viewportColorView()`/`viewportSampler()`/`viewportExtent()` expose it; `resizeViewport` recreates it + rewrites the blit descriptor.
2. `VkPostProcess::blitToSwapchain` draws a full-screen blit of the viewport image, built against the swapchain pass.
3. `VulkanRenderer::endFrame` composites the scene + records debug lines + HUD into the viewport pass, then the swapchain pass blits the viewport image and draws ImGui.
4. `VulkanRenderer::viewportColorView()/viewportSampler()/resizeViewport()` are available (engine-internal).
5. The sandbox renders **identically** to pre-M43a (scene, selection outline, gizmo, collider wireframes, HUD, Play mode, post-effects, window resize) — confirmed in the visual gate.
6. 54/54 CTest cases green; no test changes; shipping games + scene/mask/glow logic untouched.

---

## Risk log

- **Render-pass compatibility (main risk).** The composite/debug-line/HUD pipelines were built against `swapchainPass()`. They only record correctly into `viewportPass_` if it is *compatible* — same attachment count (2), same color format (`colorFormat_` = swapchain color), same depth format (`depthFormat_` = swapchain depth), same sample counts. Task 1 must match these exactly. If validation complains about render-pass incompatibility, the formats/attachment-count diverged.
- **Depth attachment necessity.** The composite is depth-less (full-screen triangle), but the pipelines were built for a color+depth pass, so `viewportPass_` MUST include the depth attachment for compatibility even though composite ignores it. Debug lines may depth-test; keeping the depth attachment preserves their existing behavior.
- **Viewport image layout for the blit.** `viewportPass_`'s color finalLayout is `SHADER_READ_ONLY_OPTIMAL`; the blit samples it in the next (swapchain) pass. The exit color subpass dependency (`COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER`) must be present so the blit reads valid data. Mirror the scene pass's color-exit dep.
- **Resize ordering.** `resizeViewport` calls `vkDeviceWaitIdle`; ensure it's only called outside frame recording (M43a calls it from swapchain-resize and, in M43b, from the panel-size change at frame top — both safe). Don't call it mid-`endFrame`.
- **descPool_ capacity.** Adding `blitDescSet_` consumes one more set + one combined-image-sampler from `descPool_`. If the pool was sized exactly, bump it (Task 2 Step 2).
- **HUD usage.** The sandbox may not use the HUD; moving `hud_.record` into the viewport pass is still correct (no-op when `pendingHudValid_` is false). Net-shooter etc. don't use `ImGuiLayer` and are unaffected (they don't run the editor host).
```
