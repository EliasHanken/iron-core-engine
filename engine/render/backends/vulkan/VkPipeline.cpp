// VkPipeline.cpp — render pass (one color + one depth attachment),
// graphics pipeline factory (one per VkShader), and per-swapchain-image
// framebuffers.

#include "render/backends/vulkan/VkPipeline.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

#include "scene/Mesh.h"
#include "scene/SkinnedMesh.h"

namespace iron {

namespace {

VkRenderPass createRenderPass(VkContext& ctx, VkFormat color, VkFormat depth) {
    VkAttachmentDescription colorAttach{};
    colorAttach.format = color;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttach{};
    depthAttach.format = depth;
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = { colorAttach, depthAttach };

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &info, nullptr, &rp));
    return rp;
}

::VkPipeline createGraphicsPipelineImpl(
        VkContext& ctx, VkSwapchain& swap, VkRenderPass rp, const VkShader& sh,
        const VkPipelineVertexInputStateCreateInfo& vi, bool wireframe) {
    // Determine whether tessellation stages are present.
    const bool tess = (sh.tescModule != VK_NULL_HANDLE);

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    // Tessellated/displaced surfaces are double-sided: the tese winding under the
    // negative-height viewport leaves them back-facing from above, and a displaced
    // heightfield is best viewed from any angle. Non-tess geometry keeps back-cull.
    rs.cullMode = tess ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dyn;

    // Build the shader stage array dynamically: vert [+ tesc + tese] + frag.
    VkPipelineShaderStageCreateInfo stages[4]{};
    uint32_t stageCount = 0;
    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stageCount].module = sh.vertexModule;
    stages[stageCount].pName  = "main";
    ++stageCount;
    if (tess) {
        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        stages[stageCount].module = sh.tescModule;
        stages[stageCount].pName  = "main";
        ++stageCount;
        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        stages[stageCount].module = sh.teseModule;
        stages[stageCount].pName  = "main";
        ++stageCount;
    }
    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stageCount].module = sh.fragmentModule;
    stages[stageCount].pName  = "main";
    ++stageCount;

    VkPipelineTessellationStateCreateInfo ts{};
    ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    ts.patchControlPoints = 3;

    (void)swap;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pTessellationState = tess ? &ts : nullptr;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dynInfo;
    info.layout = sh.pipelineLayout;
    info.renderPass = rp;
    info.subpass = 0;

    ::VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &info,
                                        nullptr, &pipeline));
    return pipeline;
}

::VkPipeline createGraphicsPipeline(VkContext& ctx, VkSwapchain& swap,
                                     VkRenderPass rp, const VkShader& sh,
                                     bool wireframe) {
    // Vertex input: position, normal, uv, tangent — match scene::Vertex.
    // Confirmed: iron::Vertex = { Vec3 position; Vec3 normal; Vec2 uv; Vec3 tangent; }
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);  // from scene/Mesh.h
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,     offsetof(Vertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT,  offsetof(Vertex, tangent)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    return createGraphicsPipelineImpl(ctx, swap, rp, sh, vi, wireframe);
}

// M23 — SkinnedVertex pipeline. 6 attributes (Pos, Normal, UV, Tangent,
// joints[uvec4], weights[vec4]) at 76-byte stride. Everything else
// (rasterization, depth, blend, render pass) matches the static path.
::VkPipeline createSkinnedGraphicsPipeline(VkContext& ctx, VkSwapchain& swap,
                                            VkRenderPass rp, const VkShader& sh,
                                            bool wireframe) {
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = static_cast<std::uint32_t>(sizeof(SkinnedVertex));
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[6]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(SkinnedVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, tangent)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(SkinnedVertex, joints)};
    attrs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SkinnedVertex, weights)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attrs;

    return createGraphicsPipelineImpl(ctx, swap, rp, sh, vi, wireframe);
}

}  // namespace

bool VkPipeline::init(VkContext& ctx, VkSwapchain& swap) {
    renderPass_ = createRenderPass(ctx, swap.colorFormat(), swap.depthFormat());
    if (!renderPass_) return false;
    return recreateFramebuffers(ctx, swap);
}

void VkPipeline::destroy(VkContext& ctx) {
    for (auto& e : pipelines_) {
        if (e.pipeline) vkDestroyPipeline(ctx.device(), e.pipeline, nullptr);
    }
    pipelines_.clear();
    for (auto& e : skinnedPipelines_) {
        if (e.pipeline) vkDestroyPipeline(ctx.device(), e.pipeline, nullptr);
    }
    skinnedPipelines_.clear();
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    framebuffers_.clear();
    if (renderPass_) {
        vkDestroyRenderPass(ctx.device(), renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
}

bool VkPipeline::recreateFramebuffers(VkContext& ctx, VkSwapchain& swap) {
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    framebuffers_.clear();
    framebuffers_.resize(swap.imageCount(), VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < swap.imageCount(); ++i) {
        VkImageView attachments[2] = { swap.colorView(i), swap.depthView() };
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.width = swap.extent().width;
        info.height = swap.extent().height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &info, nullptr, &framebuffers_[i]));
        if (!framebuffers_[i]) return false;
    }
    return true;
}

::VkPipeline VkPipeline::pipelineFor(VkContext& ctx, VkSwapchain& swap,
                                      const VkShader& sh, bool wireframe) {
    for (const auto& e : pipelines_) {
        if (e.shader == &sh && e.wireframe == wireframe) return e.pipeline;
    }
    // Built against scenePass_ — scene geometry records there, not the
    // swapchain pass (see setScenePass / VUID-02684).
    auto p = createGraphicsPipeline(ctx, swap, scenePass_, sh, wireframe);
    pipelines_.push_back({&sh, wireframe, p});
    return p;
}

::VkPipeline VkPipeline::skinnedPipelineFor(VkContext& ctx, VkSwapchain& swap,
                                              const VkShader& sh, bool wireframe) {
    for (const auto& e : skinnedPipelines_) {
        if (e.shader == &sh && e.wireframe == wireframe) return e.pipeline;
    }
    auto p = createSkinnedGraphicsPipeline(ctx, swap, scenePass_, sh, wireframe);
    skinnedPipelines_.push_back({&sh, wireframe, p});
    return p;
}

void VkPipeline::invalidate(VkContext& ctx, const VkShader* sh) {
    // Drop all PipelineEntry entries for this shader.
    for (auto it = pipelines_.begin(); it != pipelines_.end(); ) {
        if (it->shader == sh) {
            if (it->pipeline) vkDestroyPipeline(ctx.device(), it->pipeline, nullptr);
            it = pipelines_.erase(it);
        } else {
            ++it;
        }
    }
    // Drop skinned pipeline entries for this shader.
    for (auto it = skinnedPipelines_.begin(); it != skinnedPipelines_.end(); ) {
        if (it->shader == sh) {
            if (it->pipeline) vkDestroyPipeline(ctx.device(), it->pipeline, nullptr);
            it = skinnedPipelines_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace iron
