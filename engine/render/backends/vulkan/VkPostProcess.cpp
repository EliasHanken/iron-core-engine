// VkPostProcess.cpp — offscreen scene-color target + full-screen copy pipeline.

#include "render/backends/vulkan/VkPostProcess.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

namespace iron {

namespace {

const char* kFullscreenVert = R"(#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kCopyFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
void main() { outColor = texture(uScene, vUV); }
)";

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VkPostProcess::init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
                         VkExtent2D extent, VkSampler sharedSampler,
                         VkRenderPass swapchainPass) {
    ctx_         = &ctx;
    colorFormat_ = colorFormat;
    depthFormat_ = depthFormat;
    extent_      = extent;
    sampler_     = sharedSampler;

    if (!createTargets(ctx)) return false;
    if (!createCopyPipeline(ctx, swapchainPass)) return false;
    return true;
}

void VkPostProcess::destroy(VkContext& ctx) {
    // Pipeline objects.
    if (copyPipeline_)   { vkDestroyPipeline(ctx.device(), copyPipeline_, nullptr); copyPipeline_ = VK_NULL_HANDLE; }
    if (copyPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), copyPipeLayout_, nullptr); copyPipeLayout_ = VK_NULL_HANDLE; }
    if (copySetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), copySetLayout_, nullptr); copySetLayout_ = VK_NULL_HANDLE; }
    // descPool_ owns copyDescSet_; destroying the pool frees the set.
    if (descPool_)       { vkDestroyDescriptorPool(ctx.device(), descPool_, nullptr); descPool_ = VK_NULL_HANDLE; copyDescSet_ = VK_NULL_HANDLE; }

    // Render target objects.
    destroyTargets(ctx);
}

bool VkPostProcess::resize(VkContext& ctx, VkExtent2D extent) {
    extent_ = extent;
    destroyTargets(ctx);
    if (!createTargets(ctx)) return false;

    // Re-write the copy descriptor set to point at the new sceneColorView_.
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler_;
    imgInfo.imageView   = sceneColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = copyDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

void VkPostProcess::beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = scenePass_;
    rpBegin.framebuffer       = sceneFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = extent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = static_cast<float>(extent_.width);
    vp.height   = static_cast<float>(extent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endScenePass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::recordComposite(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copyPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            copyPipeLayout_, 0, 1, &copyDescSet_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool VkPostProcess::createTargets(VkContext& ctx) {
    // --- Scene color image (colorFormat_, COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = colorFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneColor_, &sceneColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = colorFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneColorView_));
    }

    // --- Scene depth image (depthFormat_, DEPTH_STENCIL_ATTACHMENT) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = depthFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneDepth_, &sceneDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = depthFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneDepthView_));
    }

    // --- Scene render pass: color + depth ---
    // Color final layout = SHADER_READ_ONLY_OPTIMAL so the composite pass
    // can sample it. Entry dep: FRAGMENT_SHADER -> COLOR_ATTACHMENT_OUTPUT
    // guards frame N+1 overwriting the image while frame N still samples it.
    // Exit dep: COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER lets the composite
    // pass sample safely.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format         = colorFormat_;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format         = depthFormat_;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    // Entry: wait for prior frame's sampling to finish before writing color.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // Exit: composite pass can sample after color writes complete.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies   = deps;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &scenePass_));

    // --- Framebuffer ---
    VkImageView views[2] = {sceneColorView_, sceneDepthView_};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = scenePass_;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments    = views;
    fbInfo.width           = extent_.width;
    fbInfo.height          = extent_.height;
    fbInfo.layers          = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &sceneFb_));

    return true;
}

void VkPostProcess::destroyTargets(VkContext& ctx) {
    if (sceneFb_)         { vkDestroyFramebuffer(ctx.device(), sceneFb_, nullptr); sceneFb_ = VK_NULL_HANDLE; }
    if (scenePass_)       { vkDestroyRenderPass(ctx.device(), scenePass_, nullptr); scenePass_ = VK_NULL_HANDLE; }
    if (sceneColorView_)  { vkDestroyImageView(ctx.device(), sceneColorView_, nullptr); sceneColorView_ = VK_NULL_HANDLE; }
    if (sceneDepthView_)  { vkDestroyImageView(ctx.device(), sceneDepthView_, nullptr); sceneDepthView_ = VK_NULL_HANDLE; }
    if (sceneColor_)      { vmaDestroyImage(ctx.allocator(), sceneColor_, sceneColorAlloc_); sceneColor_ = VK_NULL_HANDLE; sceneColorAlloc_ = VK_NULL_HANDLE; }
    if (sceneDepth_)      { vmaDestroyImage(ctx.allocator(), sceneDepth_, sceneDepthAlloc_); sceneDepth_ = VK_NULL_HANDLE; sceneDepthAlloc_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::createCopyPipeline(VkContext& ctx, VkRenderPass swapchainPass) {
    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCopyFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler (FS).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &copySetLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &copySetLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &copyPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings or attributes — positions come from gl_VertexIndex.

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
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = copyPipeLayout_;
    pInfo.renderPass          = swapchainPass;
    pInfo.subpass             = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo,
                                       nullptr, &copyPipeline_));

    // Shader modules are no longer needed after pipeline creation.
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);

    // Descriptor pool: one set with one combined-image-sampler.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 1;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &descPool_));

    // Allocate and write the copy descriptor set.
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &copySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &copyDescSet_));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler_;
    imgInfo.imageView   = sceneColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = copyDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

}  // namespace iron
