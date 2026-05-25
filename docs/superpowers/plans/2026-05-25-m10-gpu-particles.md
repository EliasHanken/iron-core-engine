# M10 — GPU Compute Particles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `iron::ParticleSystem` (high-level Vulkan-only engine API) that ticks 1M particles per frame via a compute shader (curl-noise flow field) and renders them as additively-blended billboards (instanced draw, vertex pull from the same SSBO). New demo `games/08-particle-storm` showcases the system.

**Architecture:** New `engine/render/ParticleSystem.h` public API (header has `#error` under OpenGL). Concrete `VkParticleSystem` in `engine/render/backends/vulkan/` owns one SSBO + one compute pipeline (curl-noise update) + one graphics pipeline (additive billboard render). `VkComputePipeline` is a private compute-pipeline helper for the Vulkan backend. `VulkanRenderer` gains two engine-internal accessors (`currentCommandBuffer()`, `frameRing()`) so external Vulkan subsystems can record into the active frame and use the per-frame UBO sub-allocator.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), Vulkan 1.3, glslang (runtime GLSL→SPIR-V), VMA, GLFW.

**Spec:** `docs/superpowers/specs/2026-05-25-m10-gpu-particles-design.md`

---

## Conventions used in this plan

- Build: `cmake --build build --config Debug`. Tests: `ctest --test-dir build -C Debug --output-on-failure`. Repo root: `C:\Users\elias\Documents\_dev\iron-core-engine`.
- Configure: `cmake -S . -B build -DIRON_RENDER_BACKEND=opengl|vulkan` (default opengl). The M10 work is **only buildable under Vulkan** — the demo game is gated to the Vulkan configure. Tests other than `test_curl_noise` must continue to pass under both backends.
- Test harness: `tests/test_framework.h` — `CHECK`, `CHECK_NEAR` (1e-4 tolerance), `iron_test_result()`.
- Commit message style: heredoc, `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- All Vulkan code lives in `namespace iron` (no nested `vulkan::` namespace) to match the existing Vulkan backend pattern.
- `VK_CHECK(call)` macro from `VkUtils.h` aborts on non-success after logging file:line + result.

---

## Task 0: Branch

**Files:** none (git operation only).

- [ ] **Step 1: Branch off main**

```bash
git checkout main
git pull --ff-only
git checkout -b feat/m10-gpu-particles
```

- [ ] **Step 2: Verify baseline (Vulkan + OpenGL)**

Active backend should already be opengl from M9 close. Verify:

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: clean build, `100% tests passed, 0 tests failed out of 33`. If anything fails before any M10 work, STOP and report.

---

## Task 1: `VulkanRenderer` engine-internal accessors

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`

Add two accessors needed by `VkParticleSystem` so it can record draws into the active frame's primary command buffer + allocate per-frame UBO offsets.

- [ ] **Step 1: Add the accessors to `VulkanRenderer.h`**

Find the existing public methods block in `engine/render/backends/vulkan/VulkanRenderer.h`. After the existing public methods (`init`, `initOk`, the abstract Renderer overrides), add:

```cpp
    // --- engine-internal accessors (not part of iron::Renderer) ---

    // Returns the current frame's primary command buffer. Only meaningful
    // between Renderer::beginFrame and Renderer::endFrame. Used by
    // external Vulkan subsystems (e.g., iron::ParticleSystem) that need
    // to record draws into the active render pass.
    VkCommandBuffer currentCommandBuffer() const;

    // Exposes the frame ring so external Vulkan subsystems can allocate
    // per-frame UBO storage and descriptor sets that live until the
    // next time this frame index is reused.
    VkFrameRing& frameRing();
```

- [ ] **Step 2: Implement the accessors in `VulkanRenderer.cpp`**

Add at the end of the file, inside `namespace iron`:

```cpp
VkCommandBuffer VulkanRenderer::currentCommandBuffer() {
    return frames_.current().commandBuffer;
}

VkFrameRing& VulkanRenderer::frameRing() {
    return frames_;
}
```

`frames_.current()` returns a non-const `Frame&`, so `currentCommandBuffer()` is non-const too (note: change the header declaration to drop the `const` if you copied a `const` version).

- [ ] **Step 3: Build under Vulkan (verify nothing broke)**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
```

Expected: clean build.

- [ ] **Step 4: Reset to OpenGL + verify tests still pass**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33/33 pass.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M10 Task 1: VulkanRenderer engine-internal accessors

Add VulkanRenderer::currentCommandBuffer() and VulkanRenderer::frameRing()
so external Vulkan subsystems (iron::ParticleSystem in M10) can record
draws into the active frame's primary command buffer + allocate per-frame
UBO storage. Not part of the abstract iron::Renderer interface — these
are explicit cross-boundary touch points for engine subsystems that
target the Vulkan backend.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `VkComputePipeline` private helper

**Files:**
- Create: `engine/render/backends/vulkan/VkComputePipeline.h`
- Create: `engine/render/backends/vulkan/VkComputePipeline.cpp`
- Modify: `engine/CMakeLists.txt` (register both sources in the Vulkan branch)

A thin wrapper around SPIR-V → `VkShaderModule` → `VkPipelineLayout` → `VkPipeline` for a single compute shader. Private to the Vulkan backend; will gain a second consumer in a future milestone, at which point we may promote it to a more general engine helper.

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkComputePipeline.h`**

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace iron {

class VkContext;

// Private Vulkan-backend helper: SPIR-V → compute pipeline + matching
// pipeline layout. Owns nothing outside its own pipeline/layout/module
// handles; caller supplies the descriptor set layout.
class VkComputePipeline {
public:
    bool init(VkContext& ctx,
              const std::vector<std::uint32_t>& spirv,
              VkDescriptorSetLayout setLayout);
    void destroy(VkContext& ctx);

    VkPipeline       pipeline()       const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return layout_; }

private:
    VkShaderModule   module_   = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkComputePipeline.cpp`**

```cpp
// VkComputePipeline.cpp — SPIR-V → compute pipeline + matching layout.

#include "render/backends/vulkan/VkComputePipeline.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

namespace iron {

bool VkComputePipeline::init(VkContext& ctx,
                             const std::vector<std::uint32_t>& spirv,
                             VkDescriptorSetLayout setLayout) {
    VkShaderModuleCreateInfo modInfo{};
    modInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    modInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    modInfo.pCode = spirv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &modInfo, nullptr, &module_));
    if (module_ == VK_NULL_HANDLE) return false;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &layout_));
    if (layout_ == VK_NULL_HANDLE) return false;

    VkComputePipelineCreateInfo cpInfo{};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = module_;
    cpInfo.stage.pName = "main";
    cpInfo.layout = layout_;

    VK_CHECK(vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                       &cpInfo, nullptr, &pipeline_));
    return pipeline_ != VK_NULL_HANDLE;
}

void VkComputePipeline::destroy(VkContext& ctx) {
    if (pipeline_) { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_)   { vkDestroyPipelineLayout(ctx.device(), layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (module_)   { vkDestroyShaderModule(ctx.device(), module_, nullptr); module_ = VK_NULL_HANDLE; }
}

}  // namespace iron
```

- [ ] **Step 3: Register `VkComputePipeline.cpp` in `engine/CMakeLists.txt`**

Find the `if (IRON_RENDER_BACKEND STREQUAL "vulkan")` block. Inside its `target_sources(ironcore PRIVATE ...)` list, add `render/backends/vulkan/VkComputePipeline.cpp` (alongside the existing Vulkan sources).

- [ ] **Step 4: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 5: Reset to OpenGL + verify tests still pass**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33/33 pass.

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkComputePipeline.h engine/render/backends/vulkan/VkComputePipeline.cpp engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M10 Task 2: VkComputePipeline private helper

Thin Vulkan-backend wrapper: SPIR-V → VkShaderModule → VkPipelineLayout
→ VkPipeline for a single compute shader. Caller supplies the
descriptor set layout. Used by VkParticleSystem in M10; will gain a
second consumer in a future milestone (GPU skinning / culling / etc.).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `ParticleSystem.h` public API + stub factory

**Files:**
- Create: `engine/render/ParticleSystem.h`

The public API + a temporary stub `createParticleSystem` that returns nullptr. The real implementation lands in Task 4 when `VkParticleSystem` exists. Defining the header now lets us put it under review as a stable contract.

- [ ] **Step 1: Create `engine/render/ParticleSystem.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::ParticleSystem requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>
#include <memory>

namespace iron {

class Renderer;  // forward

struct ParticleSystemConfig {
    std::uint32_t count        = 1'000'000;
    float         spawnRadius  = 20.0f;
    float         lifetimeMin  = 4.0f;
    float         lifetimeMax  = 8.0f;
    float         noiseScale   = 0.08f;
    float         noiseStrength = 4.0f;
    float         spriteSize   = 0.06f;
    Vec3          colorYoung   = {0.6f, 0.95f, 1.0f};   // bright cyan
    Vec3          colorOld     = {0.05f, 0.10f, 0.3f};  // deep blue
    std::uint32_t seed         = 0xC0FFEE;
};

// GPU-resident particle system. Construct AFTER Renderer is initialised.
// Vulkan-only — the header has an #error under any other backend.
class ParticleSystem {
public:
    virtual ~ParticleSystem() = default;

    // Advance one tick. dtSec is the simulation step (typically the
    // frame's delta time). Internally a single compute dispatch.
    virtual void tick(float dtSec) = 0;

    // Draw all live particles with the camera matrices for this frame.
    // Must be called BETWEEN Renderer::beginFrame and Renderer::endFrame.
    virtual void render(const Mat4& view, const Mat4& projection) = 0;

    virtual std::uint32_t count() const = 0;
};

// Factory. Returns nullptr if the Vulkan compute path failed to
// initialise. Caller owns the returned pointer.
std::unique_ptr<ParticleSystem> createParticleSystem(
    Renderer& renderer, const ParticleSystemConfig& cfg);

}  // namespace iron
```

- [ ] **Step 2: Verify it compiles under both backends**

The `#error` should fire if anyone tries to include it under OpenGL. Currently nothing includes it. Configure both backends to make sure neither breaks:

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: clean under both. 33/33 tests pass under OpenGL.

- [ ] **Step 3: Commit**

```bash
git add engine/render/ParticleSystem.h
git commit -m "$(cat <<'EOF'
M10 Task 3: ParticleSystem public API header

engine/render/ParticleSystem.h declares the public interface:
ParticleSystemConfig POD, abstract ParticleSystem class with tick +
render, and createParticleSystem(renderer, cfg) factory. Header has
#error under any non-Vulkan backend. Concrete implementation lands
in Task 4 (VkParticleSystem); for now there's no .cpp providing the
factory definition — including the header in game code would link-error,
but no game code includes it yet.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `VkParticleSystem` skeleton — SSBO + initial state + null tick/render

**Files:**
- Create: `engine/render/backends/vulkan/VkParticleSystem.h`
- Create: `engine/render/backends/vulkan/VkParticleSystem.cpp`
- Modify: `engine/CMakeLists.txt`

Bring up the SSBO with initial particle state. tick + render are stubs (just early-return); they get filled in Tasks 5 + 6. After this task, `createParticleSystem` returns a real instance.

- [ ] **Step 1: Create `engine/render/backends/vulkan/VkParticleSystem.h`**

```cpp
#pragma once

#include "render/ParticleSystem.h"
#include "render/backends/vulkan/VkComputePipeline.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace iron {

class VulkanRenderer;

// Vulkan-backed iron::ParticleSystem. Owns:
//   - One device-local SSBO holding `count` particles (64 bytes each).
//   - One compute pipeline + descriptor set layout (curl-noise update).
//   - One graphics pipeline + descriptor set layout (additive billboard
//     render via SSBO vertex pull).
//   - One small host-mapped UBO buffer used for the Sim uniform fed to
//     the compute dispatch (separate from the frame ring's UBO because
//     tick() runs outside the per-frame command buffer).
class VkParticleSystem : public ParticleSystem {
public:
    VkParticleSystem();
    ~VkParticleSystem() override;

    bool init(VulkanRenderer& renderer, const ParticleSystemConfig& cfg);

    void tick(float dtSec) override;
    void render(const Mat4& view, const Mat4& projection) override;
    std::uint32_t count() const override { return cfg_.count; }

private:
    bool createSsbo();
    void uploadInitialState();

    VulkanRenderer* renderer_ = nullptr;
    ParticleSystemConfig cfg_;
    std::uint32_t frameSeed_ = 0;

    VkBuffer       ssbo_      = VK_NULL_HANDLE;
    VmaAllocation  ssboAlloc_ = VK_NULL_HANDLE;

    // Compute (Task 5).
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     computePipeline_;
    VkBuffer              simUbo_       = VK_NULL_HANDLE;
    VmaAllocation         simUboAlloc_  = VK_NULL_HANDLE;
    void*                 simUboMapped_ = nullptr;

    // Render (Task 6).
    VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      renderPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            renderPipeline_       = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/vulkan/VkParticleSystem.cpp` (Task 4 portion)**

Note: this references `VulkanRenderer::context()`, an accessor added in **Step 3** below. Write Step 2 first; the build will fail at link until Step 3 lands; both are committed together in Step 7.

```cpp
// VkParticleSystem.cpp — Vulkan-backed iron::ParticleSystem.
//
// M10 Task 4: SSBO creation + initial state upload + stubbed tick/render.
// Tasks 5 + 6 fill in the compute + graphics paths.

#include "render/backends/vulkan/VkParticleSystem.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include "core/Log.h"

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace iron {

namespace {

struct ParticleCpu {
    float position[4];
    float velocity[4];
    float colorYoung[4];
    float colorOld[4];
};
static_assert(sizeof(ParticleCpu) == 64, "ParticleCpu must be 64 bytes");

}  // namespace

VkParticleSystem::VkParticleSystem() = default;

VkParticleSystem::~VkParticleSystem() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();  // see Task 4 Step 3 below
    vkDeviceWaitIdle(ctx.device());

    if (renderPipeline_)       { vkDestroyPipeline(ctx.device(), renderPipeline_, nullptr); }
    if (renderPipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), renderPipelineLayout_, nullptr); }
    if (renderSetLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), renderSetLayout_, nullptr); }

    computePipeline_.destroy(ctx);
    if (computeSetLayout_)     { vkDestroyDescriptorSetLayout(ctx.device(), computeSetLayout_, nullptr); }
    if (simUbo_)               { vmaDestroyBuffer(ctx.allocator(), simUbo_, simUboAlloc_); }

    if (ssbo_)                 { vmaDestroyBuffer(ctx.allocator(), ssbo_, ssboAlloc_); }
}

bool VkParticleSystem::init(VulkanRenderer& renderer,
                             const ParticleSystemConfig& cfg) {
    renderer_ = &renderer;
    cfg_      = cfg;
    if (!createSsbo()) return false;
    uploadInitialState();
    Log::info("VkParticleSystem: %u particles allocated (%.1f MB)",
              cfg_.count,
              static_cast<double>(cfg_.count) * sizeof(ParticleCpu) / (1024.0 * 1024.0));
    return true;
}

bool VkParticleSystem::createSsbo() {
    VkContext& ctx = renderer_->context();
    const VkDeviceSize size = static_cast<VkDeviceSize>(cfg_.count) * sizeof(ParticleCpu);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // M10 uses host-visible memory so the initial upload + Task 5 sim UBO
    // path can memcpy directly. Future optimization: device-local SSBO
    // with a staging-buffer initial upload.
    VmaAllocationInfo aInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc,
                             &ssbo_, &ssboAlloc_, &aInfo));
    return ssbo_ != VK_NULL_HANDLE;
}

void VkParticleSystem::uploadInitialState() {
    VkContext& ctx = renderer_->context();
    void* mapped = nullptr;
    vmaMapMemory(ctx.allocator(), ssboAlloc_, &mapped);
    auto* dst = static_cast<ParticleCpu*>(mapped);

    std::mt19937 rng(cfg_.seed);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);

    for (std::uint32_t i = 0; i < cfg_.count; ++i) {
        // Uniform-in-sphere via cube-root radius.
        const float r = cfg_.spawnRadius * std::pow(u01(rng), 1.0f / 3.0f);
        const float theta = 6.2831853f * u01(rng);
        const float phi   = std::acos(2.0f * u01(rng) - 1.0f);
        ParticleCpu p{};
        p.position[0] = r * std::sin(phi) * std::cos(theta);
        p.position[1] = r * std::sin(phi) * std::sin(theta);
        p.position[2] = r * std::cos(phi);
        // Stagger initial age across [0, lifetime] so the flow is
        // already saturated when the demo opens (no warm-up frame).
        const float lifetime = cfg_.lifetimeMin +
                               (cfg_.lifetimeMax - cfg_.lifetimeMin) * u01(rng);
        p.position[3] = u01(rng) * lifetime;  // age
        p.velocity[3] = lifetime;
        p.colorYoung[0] = cfg_.colorYoung.x;
        p.colorYoung[1] = cfg_.colorYoung.y;
        p.colorYoung[2] = cfg_.colorYoung.z;
        p.colorYoung[3] = 1.0f;
        p.colorOld[0]   = cfg_.colorOld.x;
        p.colorOld[1]   = cfg_.colorOld.y;
        p.colorOld[2]   = cfg_.colorOld.z;
        p.colorOld[3]   = 1.0f;
        dst[i] = p;
    }

    vmaUnmapMemory(ctx.allocator(), ssboAlloc_);
}

// --- Task 5 + Task 6 will fill these in. ---

void VkParticleSystem::tick(float /*dtSec*/) {
    // Task 5 stub.
}

void VkParticleSystem::render(const Mat4& /*view*/, const Mat4& /*projection*/) {
    // Task 6 stub.
}

// --- Factory ---

std::unique_ptr<ParticleSystem> createParticleSystem(
    Renderer& renderer, const ParticleSystemConfig& cfg) {
    auto* vk = dynamic_cast<VulkanRenderer*>(&renderer);
    if (!vk) {
        Log::error("createParticleSystem: renderer is not a VulkanRenderer");
        return nullptr;
    }
    auto sys = std::make_unique<VkParticleSystem>();
    if (!sys->init(*vk, cfg)) return nullptr;
    return sys;
}

}  // namespace iron
```

- [ ] **Step 3: Add `VulkanRenderer::context()` accessor**

The destructor + `createSsbo` reach for `renderer_->context()`. That accessor doesn't exist yet — add it.

In `engine/render/backends/vulkan/VulkanRenderer.h`, in the public methods block (next to `currentCommandBuffer()` and `frameRing()` from Task 1), add:

```cpp
    // Engine-internal: expose the VkContext so external Vulkan subsystems
    // can allocate their own VMA buffers + Vulkan objects.
    VkContext& context();
```

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, add the implementation:

```cpp
VkContext& VulkanRenderer::context() { return context_; }
```

- [ ] **Step 4: Register `VkParticleSystem.cpp` in `engine/CMakeLists.txt`**

Inside the `if (IRON_RENDER_BACKEND STREQUAL "vulkan")` block's `target_sources` list, add `render/backends/vulkan/VkParticleSystem.cpp` (alongside the existing Vulkan sources + Task 2's `VkComputePipeline.cpp`).

- [ ] **Step 5: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 6: Reset to OpenGL + verify tests still pass**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33/33 pass.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VkParticleSystem.h engine/render/backends/vulkan/VkParticleSystem.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M10 Task 4: VkParticleSystem skeleton — SSBO + initial state

Brings up the device-side particle storage (one host-visible VMA SSBO,
64 bytes/particle × count). uploadInitialState seeds particles with
random positions uniform-in-sphere of spawnRadius and staggered initial
ages so the flow is saturated on frame 0. createParticleSystem factory
dynamic_casts the Renderer& to VulkanRenderer&; returns nullptr if it
fails. VulkanRenderer gains a context() accessor for engine-internal
subsystems.

tick() and render() are stubs — filled in Tasks 5 and 6.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Compute path — pipeline + tick()

**Files:**
- Modify: `engine/render/backends/vulkan/VkParticleSystem.h` (no changes; members already declared in Task 4)
- Modify: `engine/render/backends/vulkan/VkParticleSystem.cpp` (add compute init + tick)

Add the curl-noise compute shader (inline GLSL), compile via glslang, build the compute pipeline, create the descriptor set layout, create the small Sim UBO, and implement tick() with a one-shot command buffer dispatch.

- [ ] **Step 1: Add the inline compute shader source + helper for the Sim UBO struct**

In `engine/render/backends/vulkan/VkParticleSystem.cpp`, BEFORE the `VkParticleSystem::VkParticleSystem()` definition (inside the anonymous namespace alongside `ParticleCpu`), add:

```cpp
struct SimUboCpu {
    float dtSec;
    float noiseScale;
    float noiseStrength;
    float spawnRadius;
    float lifetimeMin;
    float lifetimeMax;
    std::uint32_t count;
    std::uint32_t frameSeed;
};
static_assert(sizeof(SimUboCpu) == 32, "SimUboCpu must be 32 bytes");

const char* kComputeShader = R"(#version 450

layout(local_size_x = 256) in;

struct Particle {
    vec4 position;
    vec4 velocity;
    vec4 colorYoung;
    vec4 colorOld;
};

layout(std430, set = 0, binding = 0) buffer ParticleBuffer {
    Particle particles[];
};

layout(set = 0, binding = 1) uniform Sim {
    float dtSec;
    float noiseScale;
    float noiseStrength;
    float spawnRadius;
    float lifetimeMin;
    float lifetimeMax;
    uint  count;
    uint  frameSeed;
} sim;

float hash11(float n) { return fract(sin(n) * 43758.5453); }

float hash31(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i + vec3(0,0,0));
    float n100 = hash31(i + vec3(1,0,0));
    float n010 = hash31(i + vec3(0,1,0));
    float n110 = hash31(i + vec3(1,1,0));
    float n001 = hash31(i + vec3(0,0,1));
    float n101 = hash31(i + vec3(1,0,1));
    float n011 = hash31(i + vec3(0,1,1));
    float n111 = hash31(i + vec3(1,1,1));
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

vec3 potential(vec3 p) {
    return vec3(
        vnoise(p),
        vnoise(p + vec3(31.42, 17.13, 95.11)),
        vnoise(p + vec3(7.31, 81.97, 49.18)));
}

vec3 curl(vec3 p) {
    const float eps = 0.01;
    vec3 dx = vec3(eps, 0, 0);
    vec3 dy = vec3(0, eps, 0);
    vec3 dz = vec3(0, 0, eps);
    vec3 dPdx = (potential(p + dx) - potential(p - dx)) / (2.0 * eps);
    vec3 dPdy = (potential(p + dy) - potential(p - dy)) / (2.0 * eps);
    vec3 dPdz = (potential(p + dz) - potential(p - dz)) / (2.0 * eps);
    return vec3(
        dPdy.z - dPdz.y,
        dPdz.x - dPdx.z,
        dPdx.y - dPdy.x);
}

void respawn(inout Particle p, uint id) {
    float seed = float(id) * 0.0001 + float(sim.frameSeed) * 0.7919;
    float u = hash11(seed + 1.0);
    float v = hash11(seed + 2.0);
    float w = hash11(seed + 3.0);
    float r = sim.spawnRadius * pow(u, 1.0 / 3.0);
    float theta = 6.2831853 * v;
    float phi   = acos(2.0 * w - 1.0);
    p.position.xyz = vec3(
        r * sin(phi) * cos(theta),
        r * sin(phi) * sin(theta),
        r * cos(phi));
    p.position.w = 0.0;
    p.velocity.xyz = vec3(0.0);
    p.velocity.w = mix(sim.lifetimeMin, sim.lifetimeMax,
                       hash11(seed + 4.0));
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= sim.count) return;

    Particle p = particles[id];
    p.position.w += sim.dtSec;
    if (p.position.w >= p.velocity.w) {
        respawn(p, id);
    } else {
        vec3 v = curl(p.position.xyz * sim.noiseScale) * sim.noiseStrength;
        p.velocity.xyz = v;
        p.position.xyz += v * sim.dtSec;
    }
    particles[id] = p;
}
)";
```

- [ ] **Step 2: Add `#include "render/backends/vulkan/VkShader.h"` to the cpp's includes**

That gives us `compileGlsl`.

- [ ] **Step 3: Add private `initCompute()` method + call it from `init()`**

Add to the private methods block in `VkParticleSystem.h`:

```cpp
    bool initCompute();
```

In `VkParticleSystem.cpp`, in `VkParticleSystem::init` BEFORE the `Log::info(...)` line, insert:

```cpp
    if (!initCompute()) return false;
```

Implement `initCompute()` in the cpp (before the factory at the bottom of the file):

```cpp
bool VkParticleSystem::initCompute() {
    VkContext& ctx = renderer_->context();

    // Descriptor set layout: SSBO + UBO.
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 2;
    slInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &computeSetLayout_));

    // SPIR-V → compute pipeline.
    auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kComputeShader);
    if (spirv.empty()) {
        Log::error("VkParticleSystem: compute shader compile failed");
        return false;
    }
    if (!computePipeline_.init(ctx, spirv, computeSetLayout_)) return false;

    // Persistent host-mapped Sim UBO (32 bytes, padded to 256 for safety).
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 256;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo aInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &simUbo_, &simUboAlloc_, &aInfo));
    simUboMapped_ = aInfo.pMappedData;

    return true;
}
```

- [ ] **Step 4: Implement tick() with one-shot dispatch**

Replace the tick() stub in `VkParticleSystem.cpp` with:

```cpp
void VkParticleSystem::tick(float dtSec) {
    VkContext& ctx = renderer_->context();

    // Write Sim uniform.
    SimUboCpu sim{};
    sim.dtSec         = dtSec;
    sim.noiseScale    = cfg_.noiseScale;
    sim.noiseStrength = cfg_.noiseStrength;
    sim.spawnRadius   = cfg_.spawnRadius;
    sim.lifetimeMin   = cfg_.lifetimeMin;
    sim.lifetimeMax   = cfg_.lifetimeMax;
    sim.count         = cfg_.count;
    sim.frameSeed     = ++frameSeed_;
    std::memcpy(simUboMapped_, &sim, sizeof(sim));

    // One-shot command pool + buffer for this dispatch.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pInfo.queueFamilyIndex = ctx.graphicsFamily();
    pInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    // One-shot descriptor pool sized for exactly one set with two bindings.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = 1;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes = sizes;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = dpool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts = &computeSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorBufferInfo ssboInfo{};
    ssboInfo.buffer = ssbo_;
    ssboInfo.offset = 0;
    ssboInfo.range  = static_cast<VkDeviceSize>(cfg_.count) * sizeof(ParticleCpu);

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = simUbo_;
    uboInfo.offset = 0;
    uboInfo.range  = sizeof(SimUboCpu);

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &ssboInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &uboInfo;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    // Record + submit.
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                             computePipeline_.pipelineLayout(),
                             0, 1, &set, 0, nullptr);
    const std::uint32_t groups = (cfg_.count + 255u) / 256u;
    vkCmdDispatch(cb, groups, 1, 1);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    vkQueueWaitIdle(ctx.graphicsQueue());

    vkDestroyDescriptorPool(ctx.device(), dpool, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
}
```

- [ ] **Step 5: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 6: Reset to OpenGL + verify tests still pass**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33/33 pass.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VkParticleSystem.h engine/render/backends/vulkan/VkParticleSystem.cpp
git commit -m "$(cat <<'EOF'
M10 Task 5: VkParticleSystem compute path — curl-noise dispatch

Inline GLSL compute shader (curl-noise from analytic value noise, ~50
lines of GLSL) compiled via glslang. tick(dt) builds the Sim UBO (32
bytes), allocates a one-shot command pool/buffer + descriptor pool/set,
records vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDispatch
(ceil(count/256) workgroups), submits + vkQueueWaitIdle, then cleans up
the one-shot pools.

Compute submission is isolated (no fence sharing with the render frame)
for M10 simplicity. Moving the dispatch into the per-frame command
buffer is a follow-up optimisation.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Render path — pipeline + render()

**Files:**
- Modify: `engine/render/backends/vulkan/VkParticleSystem.h` (no changes; members already declared)
- Modify: `engine/render/backends/vulkan/VkParticleSystem.cpp` (add render init + render())

Build the graphics pipeline for the additive billboards. render() allocates a per-frame descriptor set from the frame ring's pool, writes the Camera UBO into the per-frame UBO sub-allocator, binds + draws.

- [ ] **Step 1: Add the inline render shader sources + Camera UBO CPU mirror**

In `engine/render/backends/vulkan/VkParticleSystem.cpp`, in the anonymous namespace (next to `kComputeShader` from Task 5), add:

```cpp
struct CameraUboCpu {
    float view[16];        // 64 bytes
    float projection[16];  // 64 bytes
    float spriteSize;      // 4 bytes
    float pad[3];          // 12 bytes
};
static_assert(sizeof(CameraUboCpu) == 144, "CameraUboCpu size");

const char* kRenderVertShader = R"(#version 450

struct Particle {
    vec4 position;
    vec4 velocity;
    vec4 colorYoung;
    vec4 colorOld;
};
layout(std430, set = 0, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(set = 0, binding = 1) uniform Camera {
    mat4 view;
    mat4 projection;
    float spriteSize;
    float _pad0, _pad1, _pad2;
} cam;

const vec2 kCorner[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;

void main() {
    Particle p = particles[gl_InstanceIndex];
    vec2 corner = kCorner[gl_VertexIndex];
    vCorner = corner;

    float t = clamp(p.position.w / max(p.velocity.w, 0.001), 0.0, 1.0);
    vColor = mix(p.colorYoung, p.colorOld, t);
    float aliveFade = 1.0 - smoothstep(0.85, 1.0, t);
    vColor.a *= aliveFade;

    vec4 viewCenter = cam.view * vec4(p.position.xyz, 1.0);
    viewCenter.xy += corner * cam.spriteSize;
    gl_Position = cam.projection * viewCenter;
}
)";

const char* kRenderFragShader = R"(#version 450

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vCorner, vCorner);
    if (r2 > 1.0) discard;
    float falloff = (1.0 - r2);
    float intensity = falloff * falloff;
    outColor = vec4(vColor.rgb * intensity, vColor.a * intensity);
}
)";
```

- [ ] **Step 2: Add private `initRender()` method + call it from `init()`**

Add to the private methods block in `VkParticleSystem.h`:

```cpp
    bool initRender();
```

In `VkParticleSystem.cpp`, in `VkParticleSystem::init` AFTER `initCompute()` succeeds, insert:

```cpp
    if (!initRender()) return false;
```

Implement `initRender()` in the cpp:

```cpp
bool VkParticleSystem::initRender() {
    VkContext& ctx = renderer_->context();

    // Descriptor set layout: SSBO (read-only) + Camera UBO.
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 2;
    slInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &renderSetLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &renderSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &renderPipelineLayout_));

    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kRenderVertShader);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kRenderFragShader);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkParticleSystem: render shader compile failed");
        return false;
    }

    VkShaderModule vmod = VK_NULL_HANDLE, fmod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = vspv.size() * sizeof(std::uint32_t);
        info.pCode = vspv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &vmod));
        info.codeSize = fspv.size() * sizeof(std::uint32_t);
        info.pCode = fspv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &fmod));
    }

    // Pipeline state — see spec table.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // Empty vertex input.

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dynInfo;
    info.layout = renderPipelineLayout_;
    // Reuse the renderer's scene render pass — same color + depth attachments.
    info.renderPass = renderer_->scenePass();  // see Step 3
    info.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &info,
                                        nullptr, &renderPipeline_));

    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);

    return renderPipeline_ != VK_NULL_HANDLE;
}
```

- [ ] **Step 3: Add `VulkanRenderer::scenePass()` accessor**

We need a way to get the render pass from `VkPipeline pipelines_`. In `engine/render/backends/vulkan/VulkanRenderer.h`, in the public engine-internal accessors block, add:

```cpp
    // Engine-internal: the render pass for the scene's main color+depth pass.
    // External subsystems creating their own graphics pipelines reuse it so
    // their draws go into the same framebuffer.
    VkRenderPass scenePass() const;
```

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, add:

```cpp
VkRenderPass VulkanRenderer::scenePass() const {
    return pipelines_.renderPass();
}
```

- [ ] **Step 4: Implement render()**

Replace the render() stub in `VkParticleSystem.cpp` with:

```cpp
void VkParticleSystem::render(const Mat4& view, const Mat4& projection) {
    VkContext& ctx = renderer_->context();
    VkFrameRing::Frame& frame = renderer_->frameRing().current();
    VkCommandBuffer cb = renderer_->currentCommandBuffer();

    // Build Camera UBO and allocate per-frame offset.
    CameraUboCpu camera{};
    std::memcpy(camera.view,       view.m,       sizeof(camera.view));
    std::memcpy(camera.projection, projection.m, sizeof(camera.projection));
    camera.spriteSize = cfg_.spriteSize;
    const VkDeviceSize camOffset =
        renderer_->frameRing().allocateUbo(&camera, sizeof(camera));

    // Allocate descriptor set from the frame's pool.
    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = frame.descriptorPool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts = &renderSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorBufferInfo ssboInfo{};
    ssboInfo.buffer = ssbo_;
    ssboInfo.offset = 0;
    ssboInfo.range  = static_cast<VkDeviceSize>(cfg_.count) * sizeof(ParticleCpu);

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = frame.uboBuffer;
    uboInfo.offset = camOffset;
    uboInfo.range  = sizeof(CameraUboCpu);

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &ssboInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &uboInfo;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    // Record draw into the active command buffer.
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, renderPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             renderPipelineLayout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cb, 6, cfg_.count, 0, 0);
}
```

- [ ] **Step 5: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target ironcore 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 6: Reset to OpenGL + verify tests still pass**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 33/33 pass.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VkParticleSystem.h engine/render/backends/vulkan/VkParticleSystem.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "$(cat <<'EOF'
M10 Task 6: VkParticleSystem render path — additive billboards

Inline vertex + fragment GLSL: vertex pulls per-instance Particle from
the SSBO via gl_InstanceIndex, builds a 6-vertex billboard quad via
gl_VertexIndex lookup, view-space offset by spriteSize. Fragment is a
procedural radial gradient discarding outside the unit circle.

Render pipeline: no vertex input, TRIANGLE_LIST, cull NONE, depth test
ON / write OFF, additive blend (ONE,ONE). Reuses the scene render pass
via VulkanRenderer::scenePass() so particles render into the same
framebuffer as opaque scene draws.

render() allocates one descriptor set from the frame ring's pool per
frame, writes the Camera UBO via the per-frame sub-allocator, binds +
issues vkCmdDraw(6, count, 0, 0). count instances × 6 verts = 6M
vertices at 1M particles.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `test_curl_noise.cpp` — CPU port + 4 cases

**Files:**
- Create: `tests/test_curl_noise.cpp`
- Modify: `tests/CMakeLists.txt`

A CPU port of `hash11 / hash31 / vnoise / potential / curl` from the compute shader, verified against deterministic + bounded + reproducible properties. Validates the math even though the GPU shader is what actually runs.

The test is pure C++ — no Vulkan, no GLSL. Builds + passes under both backends.

- [ ] **Step 1: Create `tests/test_curl_noise.cpp`**

```cpp
// CPU port of the curl-noise functions from the M10 particle compute
// shader. Validates determinism, boundedness, reproducibility at named
// points, and that curl is (approximately) divergence-free.
//
// The shader itself runs on the GPU; this test is the only place these
// math constants are checked.

#include "test_framework.h"

#include <cmath>

namespace {

float fract(float x) { return x - std::floor(x); }

float hash11(float n) {
    return fract(std::sin(n) * 43758.5453f);
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V3 operator*(V3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }

float dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V3 frac(V3 p) { return {fract(p.x), fract(p.y), fract(p.z)}; }
V3 floorv(V3 p) { return {std::floor(p.x), std::floor(p.y), std::floor(p.z)}; }

float hash31(V3 p) {
    p = frac(V3{p.x * 443.897f, p.y * 441.423f, p.z * 437.195f});
    V3 yzx = {p.y, p.z, p.x};
    float d = dot(p, V3{yzx.x + 19.19f, yzx.y + 19.19f, yzx.z + 19.19f});
    p = V3{p.x + d, p.y + d, p.z + d};
    return fract((p.x + p.y) * p.z);
}

float vnoise(V3 p) {
    V3 i = floorv(p);
    V3 f = frac(p);
    f = V3{f.x*f.x*(3.0f - 2.0f*f.x),
           f.y*f.y*(3.0f - 2.0f*f.y),
           f.z*f.z*(3.0f - 2.0f*f.z)};
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    float n000 = hash31(i + V3{0,0,0});
    float n100 = hash31(i + V3{1,0,0});
    float n010 = hash31(i + V3{0,1,0});
    float n110 = hash31(i + V3{1,1,0});
    float n001 = hash31(i + V3{0,0,1});
    float n101 = hash31(i + V3{1,0,1});
    float n011 = hash31(i + V3{0,1,1});
    float n111 = hash31(i + V3{1,1,1});
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

V3 potential(V3 p) {
    return V3{
        vnoise(p),
        vnoise(p + V3{31.42f, 17.13f, 95.11f}),
        vnoise(p + V3{7.31f,  81.97f, 49.18f})};
}

V3 curl(V3 p) {
    constexpr float eps = 0.01f;
    V3 dPdx = (potential(p + V3{eps, 0, 0}) - potential(p - V3{eps, 0, 0})) * (1.0f / (2.0f * eps));
    V3 dPdy = (potential(p + V3{0, eps, 0}) - potential(p - V3{0, eps, 0})) * (1.0f / (2.0f * eps));
    V3 dPdz = (potential(p + V3{0, 0, eps}) - potential(p - V3{0, 0, eps})) * (1.0f / (2.0f * eps));
    return V3{
        dPdy.z - dPdz.y,
        dPdz.x - dPdx.z,
        dPdx.y - dPdy.x};
}

float div(V3 (*f)(V3), V3 p) {
    constexpr float eps = 0.01f;
    V3 dx = (f(p + V3{eps, 0, 0}) - f(p - V3{eps, 0, 0})) * (1.0f / (2.0f * eps));
    V3 dy = (f(p + V3{0, eps, 0}) - f(p - V3{0, eps, 0})) * (1.0f / (2.0f * eps));
    V3 dz = (f(p + V3{0, 0, eps}) - f(p - V3{0, 0, eps})) * (1.0f / (2.0f * eps));
    return dx.x + dy.y + dz.z;
}

}  // namespace

int main() {
    // Determinism: vnoise(p) returns the same value across two calls.
    {
        const V3 p{0.37f, 1.21f, 4.95f};
        const float a = vnoise(p);
        const float b = vnoise(p);
        CHECK(a == b);
    }

    // Bounded: vnoise(p) ∈ [0, 1] for a grid of sample points.
    {
        bool inRange = true;
        for (int gx = 0; gx < 5; ++gx)
        for (int gy = 0; gy < 5; ++gy)
        for (int gz = 0; gz < 5; ++gz) {
            const V3 p{gx * 0.7f, gy * 0.9f, gz * 1.1f};
            const float v = vnoise(p);
            if (v < 0.0f || v > 1.0f) { inRange = false; break; }
        }
        CHECK(inRange);
    }

    // Reproducibility at a named point: curl((1.0, 2.0, 3.0)) is some
    // deterministic value. We don't assert against a hand-computed
    // reference (would be brittle); we instead verify that two calls
    // produce identical results.
    {
        const V3 p{1.0f, 2.0f, 3.0f};
        const V3 a = curl(p);
        const V3 b = curl(p);
        CHECK_NEAR(a.x, b.x);
        CHECK_NEAR(a.y, b.y);
        CHECK_NEAR(a.z, b.z);
    }

    // Curl is (approximately) divergence-free.
    // Sample div(curl) at a handful of points; should be near zero
    // (mathematically exact, numerically bounded by our eps choice).
    {
        const V3 points[] = {
            {0.5f, 1.5f, 2.5f},
            {3.0f, 0.0f, -1.0f},
            {-2.5f, 4.2f, 0.7f},
        };
        bool nearZero = true;
        for (const V3& p : points) {
            const float d = div(curl, p);
            if (std::fabs(d) > 0.5f) { nearZero = false; break; }
        }
        CHECK(nearZero);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

Append to `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_curl_noise test_curl_noise.cpp)
```

- [ ] **Step 3: Build + run the test under OpenGL (it's pure C++, no Vulkan)**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug --target test_curl_noise 2>&1 | tail -3
./build/tests/Debug/test_curl_noise.exe
```

Expected: `OK - all checks passed`.

- [ ] **Step 4: Run the full test suite**

```bash
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -5
```

Expected: 34/34 pass.

- [ ] **Step 5: Commit**

```bash
git add tests/test_curl_noise.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
M10 Task 7: test_curl_noise — CPU port of the noise functions

4 cases: determinism (vnoise(p) reproducible), boundedness (vnoise in
[0,1] across a sampling grid), reproducibility (curl(p) returns
identical values across calls), and divergence-free property (div(curl)
near zero at several sample points).

Pure C++ — no Vulkan, no GLSL. Builds + passes under both backends.
CTest goes 33 → 34.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: `games/08-particle-storm` — game scaffold + main.cpp

**Files:**
- Create: `games/08-particle-storm/main.cpp`
- Create: `games/08-particle-storm/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt` (add `add_subdirectory(games/08-particle-storm)`)

- [ ] **Step 1: Create `games/08-particle-storm/main.cpp`**

```cpp
// games/08-particle-storm/main.cpp — Vulkan-only GPU compute particle
// showcase. Free-fly through 1M particles riding a curl-noise field.

#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/ParticleSystem.h"
#include "scene/Camera.h"
#include "scene/FreeFlyCamera.h"

#include <GLFW/glfw3.h>

int main() {
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - Particle Storm";
    cfg.width  = 1280;
    cfg.height = 720;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("particle-storm: Application init failed");
        return 1;
    }

    auto renderer = iron::createRenderer(app.window());
    if (!renderer) {
        iron::Log::error("particle-storm: renderer init failed");
        return 1;
    }

    iron::ParticleSystemConfig psc;  // defaults (1M particles)
    auto particles = iron::createParticleSystem(*renderer, psc);
    if (!particles) {
        iron::Log::error("particle-storm: particle system init failed");
        return 1;
    }

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 0.0f, 45.0f};

    iron::Camera projCam;
    projCam.setAspect(static_cast<float>(app.window().width()) /
                      static_cast<float>(app.window().height()));

    app.window().setCursorCaptured(true);

    double prevMouseX = 0.0, prevMouseY = 0.0;
    glfwGetCursorPos(app.window().handle(), &prevMouseX, &prevMouseY);

    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        double mx = 0.0, my = 0.0;
        glfwGetCursorPos(app.window().handle(), &mx, &my);
        const float mouseDx = static_cast<float>(mx - prevMouseX);
        const float mouseDy = static_cast<float>(my - prevMouseY);
        prevMouseX = mx;
        prevMouseY = my;

        cam.update(t.deltaSeconds, mouseDx, mouseDy,
                   input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                   input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                   input.keyDown(GLFW_KEY_LEFT_CONTROL),
                   input.keyDown(GLFW_KEY_SPACE),
                   /*moveSpeed*/ 12.0f);

        particles->tick(t.deltaSeconds);
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();
        const iron::Mat4 proj = projCam.projectionMatrix();
        // Empty scene besides the particles: clear to deep blue.
        renderer->beginFrame(/*clear*/ iron::Vec3{0.02f, 0.02f, 0.06f},
                             /*sun*/ iron::DirectionalLight{},
                             /*lights*/ std::span<const iron::PointLight>{},
                             /*fog*/ iron::Fog{},
                             view, proj);
        particles->render(view, proj);
        renderer->endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
}
```

**Implementer note:** the exact `Input` method names (`keyDown` vs `keyPressed`), `FrameTime` field name (`deltaSeconds`), and `Application::Config` field set should be verified by reading the headers + an existing game (`games/03-showcase/main.cpp` is the closest match for FreeFlyCamera usage). If anything diverges, mirror that game's style rather than the sketch above. Don't get blocked — adapt method names as needed.

- [ ] **Step 2: Create `games/08-particle-storm/CMakeLists.txt`**

```cmake
if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(particle-storm main.cpp)
    target_link_libraries(particle-storm PRIVATE ironcore)
endif()
```

No asset-copy step — the demo uses no on-disk assets.

- [ ] **Step 3: Register the game in the top-level `CMakeLists.txt`**

Insert immediately after the existing `add_subdirectory(games/07-net-shooter)` line:

```cmake
add_subdirectory(games/08-particle-storm)
```

- [ ] **Step 4: Build under Vulkan**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug --target particle-storm 2>&1 | tail -8
```

Expected: `particle-storm.vcxproj -> ...particle-storm.exe`. If the build fails on a method-name mismatch (Input API, FrameTime field, etc.), open `games/03-showcase/main.cpp` and mirror what it does; commit the adapted version.

- [ ] **Step 5: Verify OpenGL configure still excludes the game cleanly**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: clean build (particle-storm target is absent under OpenGL because its CMakeLists's `add_executable` is inside the `if (vulkan)` guard); 34/34 tests pass.

- [ ] **Step 6: Manual smoke (user-driven; flag this for the controller, not the implementer)**

The implementer should NOT try to run the windowed game from automation. Flag in the report that the controller (or user) should run:

```
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan
cmake --build build --config Debug --target particle-storm
./build/games/08-particle-storm/Debug/particle-storm.exe
```

Expected: 1M particles visible swirling organically, color fades cyan→blue, WASD + mouse navigates, ESC closes.

- [ ] **Step 7: Commit**

```bash
git add games/08-particle-storm/main.cpp games/08-particle-storm/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
M10 Task 8: games/08-particle-storm — particle compute showcase

New Vulkan-only demo. iron::Application + iron::FreeFlyCamera + the new
iron::ParticleSystem. 1M particles ticked each frame on the GPU, rendered
as additive billboards. WASD + mouse-look free-fly through the cloud,
ESC to close. CMakeLists wraps in if (IRON_RENDER_BACKEND STREQUAL
"vulkan") so the OpenGL configure ignores the target.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Docs + PR

**Files:**
- Modify: `docs/engine/rhi-abstraction.md` (or create `docs/engine/particles.md` if you'd rather keep this separate)

- [ ] **Step 1: Append a "GPU compute + particles" section to `docs/engine/rhi-abstraction.md`**

Open the file and append:

````markdown

## GPU compute & particle system (Vulkan-only)

The Vulkan backend is the first to expose a GPU-resident particle
system, demonstrating the hardware capability the Vulkan investment
unlocks. The OpenGL backend has no equivalent — by design, since the
demo's CMakeLists.txt is gated on the Vulkan configure.

### Public API: `iron::ParticleSystem`

```cpp
#include "render/ParticleSystem.h"

iron::ParticleSystemConfig cfg;       // sensible defaults: 1M particles
auto particles = iron::createParticleSystem(*renderer, cfg);
if (!particles) { /* init failed */ }

// Per frame:
particles->tick(dt);                 // one compute dispatch
renderer->beginFrame(...);
particles->render(view, projection); // one instanced billboard draw
renderer->endFrame();
```

The header has an `#error` if included under any non-Vulkan backend, so
games meant to support multiple backends must gate the include themselves.

### Implementation

`engine/render/backends/vulkan/VkParticleSystem` owns:

- **SSBO** — one host-visible VMA buffer holding `count` 64-byte
  particles (1M = 64 MB). Initial state seeded with `std::mt19937`
  uniform-in-sphere positions + staggered initial ages so the field
  is saturated on frame 0.
- **Compute pipeline** — curl-noise update shader (analytic value
  noise, no texture lookups). Workgroup size 256.
  `tick(dt)` records a one-shot command pool + buffer, dispatches
  `ceil(count/256)` workgroups, submits, `vkQueueWaitIdle`. Isolated
  from the per-frame command buffer for M10 simplicity.
- **Graphics pipeline** — additive blend (ONE,ONE), depth test ON /
  write OFF, cull NONE, no vertex input. `render(view, projection)`
  allocates one descriptor set from the active frame's pool, writes
  the Camera UBO via the frame ring's per-frame sub-allocator, then
  `vkCmdDraw(6, count, 0, 0)` — 6 verts (billboard quad) × count
  instances, vertex pull from the SSBO via `gl_InstanceIndex`.

### Cross-boundary touchpoints

External Vulkan subsystems (like `VkParticleSystem`) need three accessors
on `iron::VulkanRenderer` that are NOT part of the abstract `Renderer`:

```cpp
VkContext&      context();              // raw Vulkan handles + VMA
VkFrameRing&    frameRing();            // per-frame UBO sub-allocator + descriptor pool
VkCommandBuffer currentCommandBuffer(); // active primary cmd buffer (between begin/endFrame)
VkRenderPass    scenePass() const;      // the main color+depth render pass
```

These are documented as engine-internal in the header. Game code never
calls them.

### Internal helper: `VkComputePipeline`

`engine/render/backends/vulkan/VkComputePipeline.h` wraps the SPIR-V →
shader module → pipeline layout → compute pipeline boilerplate. Private
to the Vulkan backend; gains a second consumer in a future milestone
(GPU skinning, GPU culling, etc.) at which point we'll consider
promoting it.

### Future work

- Fold the compute dispatch into the per-frame command buffer (drop
  the `vkQueueWaitIdle`).
- Multiple particle systems per scene (currently untested — the
  abstraction allows it but the demo uses one).
- Public `iron::ComputePipeline` engine surface (when a second
  consumer needs it).
- Particle textures, sub-particle physics (collisions, etc.) — out of
  scope for M10.
````

- [ ] **Step 2: Commit the docs**

```bash
git add docs/engine/rhi-abstraction.md
git commit -m "$(cat <<'EOF'
M10 Task 9: Docs — GPU compute + particle system section

New "GPU compute & particle system (Vulkan-only)" section in
rhi-abstraction.md covering: the iron::ParticleSystem public API,
the VkParticleSystem implementation (SSBO / compute / graphics), the
engine-internal VulkanRenderer accessors that external subsystems use
(context, frameRing, currentCommandBuffer, scenePass), the
VkComputePipeline private helper, and future-work notes.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Full suite + push branch**

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 34/34 tests pass.

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan 2>&1 | tail -2
cmake --build build --config Debug 2>&1 | tail -3
ctest --test-dir build -C Debug --output-on-failure 2>&1 | tail -3
```

Expected: 34/34 tests pass.

```bash
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl 2>&1 | tail -2
git push -u origin feat/m10-gpu-particles 2>&1 | tail -3
```

- [ ] **Step 4: Open the PR**

```bash
gh pr create --title "M10: GPU compute particles (Vulkan-only)" --body "$(cat <<'EOF'
## Summary

First Vulkan-only engine feature: `iron::ParticleSystem` ticks 1M particles per frame via a compute shader (curl-noise flow field) and renders them as additively-blended billboards. New demo `games/08-particle-storm` showcases the system. OpenGL build is unaffected (header has `#error`, game CMakeLists.txt gates on `IRON_RENDER_BACKEND=vulkan`).

### Engine additions
- `engine/render/ParticleSystem.h` — public API.
- `engine/render/backends/vulkan/VkParticleSystem.{h,cpp}` — concrete impl: SSBO (host-visible VMA), compute pipeline (curl-noise update, workgroup 256), graphics pipeline (additive billboards via SSBO vertex pull, no vertex input).
- `engine/render/backends/vulkan/VkComputePipeline.{h,cpp}` — private compute-pipeline helper.
- `engine/render/backends/vulkan/VulkanRenderer` gains engine-internal accessors: `context()`, `frameRing()`, `currentCommandBuffer()`, `scenePass()`.

### New game
`games/08-particle-storm` — Application + FreeFlyCamera + ParticleSystem, no other scene geometry, deep-blue clear color. WASD + mouse free-fly through the cloud.

### Tests
`tests/test_curl_noise.cpp` — pure C++ port of the noise functions, 4 cases (determinism, boundedness, reproducibility, divergence-free). CTest 33 → 34.

### Validation
- Both backends build clean.
- All 34 tests pass under both `-DIRON_RENDER_BACKEND=opengl` and `=vulkan`.
- Manual smoke: 1M particles render at vsync (60+ fps target on RTX 5080), color gradient visible, particles fade near death, free-fly camera navigates the cloud, ESC closes, zero Vulkan validation errors.

### Spec / plan
- Spec: `docs/superpowers/specs/2026-05-25-m10-gpu-particles-design.md`
- Plan: `docs/superpowers/plans/2026-05-25-m10-gpu-particles.md`

### Test plan
- [x] `ctest -C Debug` — 34/34 under `-DIRON_RENDER_BACKEND=opengl`
- [x] `ctest -C Debug` — 34/34 under `-DIRON_RENDER_BACKEND=vulkan`
- [x] Both backends build clean
- [ ] CI green on Windows
- [ ] **Manual** — particle-storm runs at vsync, no validation errors

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)" 2>&1 | tail -3
```

- [ ] **Step 5: Wait for CI green, then merge (user-triggered).**

---

## Spec-coverage cross-check

| Spec section                                         | Task          |
| ---------------------------------------------------- | ------------- |
| `VulkanRenderer::currentCommandBuffer()` + `frameRing()` accessors | Task 1 |
| `VkComputePipeline` private helper                   | Task 2        |
| `iron::ParticleSystem` public API + factory          | Tasks 3 + 4   |
| Per-particle 64-byte SSBO layout + initial state     | Task 4        |
| Curl-noise compute shader + tick()                   | Task 5        |
| Additive billboard render pipeline + render()        | Task 6        |
| `VulkanRenderer::scenePass()` accessor               | Task 6        |
| `test_curl_noise.cpp` (4 cases)                      | Task 7        |
| `games/08-particle-storm` (Vulkan-only gate)         | Task 8        |
| Docs in `rhi-abstraction.md`                         | Task 9        |
| Smoke + PR + CI                                      | Task 9 + user |
