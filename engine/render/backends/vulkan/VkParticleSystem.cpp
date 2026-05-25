// VkParticleSystem.cpp — Vulkan-backed iron::ParticleSystem.
//
// M10 Task 4: SSBO creation + initial state upload + stubbed tick/render.
// M10 Task 5: Compute pipeline (curl-noise GLSL) + real tick() dispatch.
// Task 6 fills in the graphics render path.

#include "render/backends/vulkan/VkParticleSystem.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VkShader.h"

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

struct SimUboCpu {
    float         dtSec;
    float         noiseScale;
    float         noiseStrength;
    float         spawnRadius;
    float         lifetimeMin;
    float         lifetimeMax;
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
    if (!initCompute()) return false;
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

bool VkParticleSystem::initCompute() {
    VkContext& ctx = renderer_->context();

    // Descriptor set layout: SSBO (binding 0) + UBO (binding 1).
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding         = 1;
    b[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 2;
    slInfo.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &computeSetLayout_));

    // Compile GLSL → SPIR-V → compute pipeline.
    auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kComputeShader);
    if (spirv.empty()) {
        Log::error("VkParticleSystem: compute shader compile failed");
        return false;
    }
    if (!computePipeline_.init(ctx, spirv, computeSetLayout_)) return false;

    // Persistent host-mapped Sim UBO (32 bytes, padded to 256 for alignment safety).
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = 256;
    bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
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

void VkParticleSystem::tick(float dtSec) {
    VkContext& ctx = renderer_->context();

    // Write Sim uniform into the persistently-mapped UBO.
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
    // Flush — no-op on coherent memory, required on non-coherent.
    vmaFlushAllocation(ctx.allocator(), simUboAlloc_, 0, sizeof(sim));

    // One-shot command pool + buffer for this dispatch.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pInfo{};
    pInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pInfo.queueFamilyIndex = ctx.graphicsFamily();
    pInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool        = pool;
    cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    // One-shot descriptor pool sized for exactly one set with two bindings.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 1;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes    = sizes;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool     = dpool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts        = &computeSetLayout_;
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
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &ssboInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo     = &uboInfo;
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
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkDestroyDescriptorPool(ctx.device(), dpool, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
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
