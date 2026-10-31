// VkGpuParticles.cpp — programmable GPU particle effects (see GpuParticles.h).

#include "render/backends/vulkan/VkGpuParticles.h"
#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace iron {

namespace {

constexpr std::uint32_t kLocalSize = 256;

// A UBO range can't be empty, so an effect without params binds this much.
constexpr std::size_t kMinParamsBytes = 16;

const char* const kVersion = "#version 450\n";

const char* const kParticleStruct = R"(
struct Particle {
    vec4 position;  // xyz, w = age
    vec4 velocity;  // xyz, w = lifetime
    vec4 a;
    vec4 b;
};
)";

const char* const kFrameBlock = R"(
layout(set = 0, binding = 1) uniform Frame {
    mat4  view;
    mat4  projection;
    vec4  cameraPos;
    float dt;
    float time;
    uint  count;
    uint  frameIndex;
} frame;
)";

const char* const kVertexMain = R"(
layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vData;

const vec2 kCorner[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main() {
    vCorner = kCorner[gl_VertexIndex];
    particleVertex(particles[gl_InstanceIndex], uint(gl_InstanceIndex), vCorner,
                   gl_Position, vColor, vData);
}
)";

const char* const kFragmentMain = R"(
layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vData;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = particleFragment(vCorner, vColor, vData);
}
)";

}  // namespace

// Mirrors the GLSL Frame block.
struct VkGpuParticles::FrameUbo {
    Mat4          view;
    Mat4          projection;
    float         cameraPos[4];
    float         dt;
    float         time;
    std::uint32_t count;
    std::uint32_t frameIndex;
};
static_assert(sizeof(Mat4) == 64, "Frame UBO expects tightly packed matrices");

GpuParticleSources composeGpuParticleSources(const GpuParticlesDesc& desc) {
    GpuParticleSources out;

    out.compute = std::string(kVersion) +
        "layout(local_size_x = " + std::to_string(kLocalSize) + ") in;\n" +
        kParticleStruct +
        "layout(std430, set = 0, binding = 0) buffer ParticleBuffer { Particle particles[]; };\n" +
        kFrameBlock + desc.commonGlsl + desc.simulateGlsl + R"(
void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= frame.count) return;
    Particle p = particles[id];
    particleSimulate(p, id);
    particles[id] = p;
}
)";

    out.vertex = std::string(kVersion) + kParticleStruct +
        "layout(std430, set = 0, binding = 0) readonly buffer ParticleBuffer { Particle particles[]; };\n" +
        kFrameBlock + desc.commonGlsl + desc.vertexGlsl + kVertexMain;

    out.fragment = std::string(kVersion) + kParticleStruct + kFrameBlock +
        desc.commonGlsl + desc.fragmentGlsl + kFragmentMain;

    return out;
}

VkGpuParticles::~VkGpuParticles() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();
    vkDeviceWaitIdle(ctx.device());

    if (renderPipeline_) vkDestroyPipeline(ctx.device(), renderPipeline_, nullptr);
    if (renderLayout_)   vkDestroyPipelineLayout(ctx.device(), renderLayout_, nullptr);
    computePipeline_.destroy(ctx);
    if (setLayout_)      vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr);
    if (ssbo_)           vmaDestroyBuffer(ctx.allocator(), ssbo_, ssboAlloc_);
}

bool VkGpuParticles::init(VulkanRenderer& renderer, const GpuParticlesDesc& desc) {
    if (desc.count == 0) {
        Log::error("GpuParticles: count must be > 0");
        return false;
    }
    if (desc.paramsSize > VkFrameRing::kUboBytesPerFrame) {
        Log::error("GpuParticles: paramsSize exceeds the per-frame UBO budget");
        return false;
    }

    desc_ = desc;
    params_.assign(std::max(desc.paramsSize, kMinParamsBytes), 0);

    const GpuParticleSources sources = composeGpuParticleSources(desc_);
    const auto computeSpv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, sources.compute);
    if (computeSpv.empty()) {
        Log::error("GpuParticles: compute shader compile failed");
        return false;
    }

    // Only assign renderer_ once compilation has passed, so the destructor has
    // nothing to tear down on that failure path.
    renderer_ = &renderer;
    if (!createBuffer() || !createSetLayout()) return false;
    if (!computePipeline_.init(renderer.context(), computeSpv, setLayout_)) return false;
    if (!createRenderPipeline(sources)) return false;

    Log::info("GpuParticles: %u particles (%.1f MB)", desc_.count,
              static_cast<double>(desc_.count) * 64.0 / (1024.0 * 1024.0));
    return true;
}

bool VkGpuParticles::createBuffer() {
    VkContext& ctx = renderer_->context();

    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = static_cast<VkDeviceSize>(desc_.count) * 64;
    info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &ssbo_, &ssboAlloc_, nullptr));
    return ssbo_ != VK_NULL_HANDLE;
}

bool VkGpuParticles::createSetLayout() {
    VkContext& ctx = renderer_->context();

    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    for (std::uint32_t i = 1; i <= 2; ++i) {
        b[i].binding         = i;
        b[i].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 3;
    info.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &info, nullptr, &setLayout_));
    return setLayout_ != VK_NULL_HANDLE;
}

bool VkGpuParticles::createRenderPipeline(const GpuParticleSources& sources) {
    VkContext& ctx = renderer_->context();

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &renderLayout_));

    const auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, sources.vertex);
    const auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, sources.fragment);
    if (vspv.empty() || fspv.empty()) {
        Log::error("GpuParticles: render shader compile failed");
        return false;
    }

    VkShaderModule vmod = VK_NULL_HANDLE, fmod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = vspv.size() * sizeof(std::uint32_t);
        info.pCode    = vspv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &vmod));
        info.codeSize = fspv.size() * sizeof(std::uint32_t);
        info.pCode    = fspv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &fmod));
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = desc_.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    const bool additive = desc_.blend == ParticleBlend::Additive;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dynInfo;
    info.layout              = renderLayout_;
    info.renderPass          = renderer_->scenePass();

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                       &renderPipeline_));

    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);
    return renderPipeline_ != VK_NULL_HANDLE;
}

VkDescriptorSet VkGpuParticles::allocateSet(const FrameUbo& ubo) {
    VkContext& ctx = renderer_->context();
    VkFrameRing& ring = renderer_->frameRing();
    VkFrameRing::Frame& frame = ring.current();

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool     = frame.descriptorPool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts        = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorBufferInfo ssboInfo{};
    ssboInfo.buffer = ssbo_;
    ssboInfo.range  = static_cast<VkDeviceSize>(desc_.count) * 64;

    VkDescriptorBufferInfo frameInfo{};
    frameInfo.buffer = frame.uboBuffer;
    frameInfo.offset = ring.allocateUbo(&ubo, sizeof(ubo));
    frameInfo.range  = sizeof(ubo);

    VkDescriptorBufferInfo paramsInfo{};
    paramsInfo.buffer = frame.uboBuffer;
    paramsInfo.offset = ring.allocateUbo(params_.data(), params_.size());
    paramsInfo.range  = params_.size();

    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                           : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    writes[0].pBufferInfo = &ssboInfo;
    writes[1].pBufferInfo = &frameInfo;
    writes[2].pBufferInfo = &paramsInfo;
    vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);
    return set;
}

void VkGpuParticles::setParams(const void* params) {
    if (desc_.paramsSize > 0) std::memcpy(params_.data(), params, desc_.paramsSize);
}

void VkGpuParticles::simulate(float dtSec) {
    VkCommandBuffer cb = renderer_->currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;  // Frame skipped (resize / acquire failure).

    if (needsClear_) {
        // The previous frame may still be simulating or drawing from this buffer.
        VkMemoryBarrier toFill{};
        toFill.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toFill.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toFill.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toFill,
                             0, nullptr, 0, nullptr);

        vkCmdFillBuffer(cb, ssbo_, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier toCompute{};
        toCompute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toCompute,
                             0, nullptr, 0, nullptr);
        needsClear_ = false;
    }

    time_ += dtSec;
    lastDt_ = dtSec;
    ++frameIndex_;

    FrameUbo ubo{};
    ubo.dt         = dtSec;
    ubo.time       = time_;
    ubo.count      = desc_.count;
    ubo.frameIndex = frameIndex_;
    VkDescriptorSet set = allocateSet(ubo);

    // Last frame's vertex shader may still be reading this buffer: frames in
    // flight share it, and the queue only orders them if we ask it to.
    VkMemoryBarrier toWrite{};
    toWrite.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toWrite.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toWrite.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toWrite,
                         0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            computePipeline_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, (desc_.count + kLocalSize - 1) / kLocalSize, 1, 1);

    VkMemoryBarrier toRead{};
    toRead.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &toRead,
                         0, nullptr, 0, nullptr);
}

void VkGpuParticles::render(const Mat4& view, const Mat4& projection) {
    if (renderer_->currentCommandBuffer() == VK_NULL_HANDLE) return;

    FrameUbo ubo{};
    ubo.view       = view;
    ubo.projection = projection;
    const Mat4 invView = iron::inverse(view);
    ubo.cameraPos[0] = invView.at(0, 3);
    ubo.cameraPos[1] = invView.at(1, 3);
    ubo.cameraPos[2] = invView.at(2, 3);
    ubo.cameraPos[3] = 1.0f;
    ubo.dt         = lastDt_;
    ubo.time       = time_;
    ubo.count      = desc_.count;
    ubo.frameIndex = frameIndex_;
    VkDescriptorSet set = allocateSet(ubo);

    renderer_->enqueueDeferredScenePass([this, set](VkCommandBuffer cb) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, renderPipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, renderLayout_, 0, 1,
                                &set, 0, nullptr);
        vkCmdDraw(cb, 6, desc_.count, 0, 0);
    });
}

std::unique_ptr<GpuParticles> createGpuParticles(Renderer& renderer,
                                                 const GpuParticlesDesc& desc) {
    auto* vk = dynamic_cast<VulkanRenderer*>(&renderer);
    if (!vk) {
        Log::error("createGpuParticles: renderer is not a VulkanRenderer");
        return nullptr;
    }
    auto effect = std::make_unique<VkGpuParticles>();
    if (!effect->init(*vk, desc)) return nullptr;
    return effect;
}

}  // namespace iron
