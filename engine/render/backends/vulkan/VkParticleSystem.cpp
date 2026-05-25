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
    float position[4];   // xyz pos, w age
    float velocity[4];   // xyz vel, w lifetime
    float colorYoung[4]; // rgb + 1 padding
    float colorOld[4];
};
static_assert(sizeof(ParticleCpu) == 64, "ParticleCpu must be 64 bytes");

}  // namespace

VkParticleSystem::VkParticleSystem() = default;

VkParticleSystem::~VkParticleSystem() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();
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
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    // M10 uses host-visible memory so the initial upload can memcpy
    // directly. NOT persistently mapped — uploadInitialState() does a
    // one-shot vmaMapMemory/Unmap; after that the SSBO is GPU-only
    // (compute writes, vertex shader reads). Future optimization:
    // device-local SSBO with a staging-buffer initial upload.
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc,
                             &ssbo_, &ssboAlloc_, nullptr));
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

    // Flush before unmap — required for non-coherent memory types.
    // No-op when VMA picked HOST_COHERENT (typical on desktop GPUs).
    vmaFlushAllocation(ctx.allocator(), ssboAlloc_, 0, VK_WHOLE_SIZE);
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
