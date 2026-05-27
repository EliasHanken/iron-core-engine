// VkReflectionTarget.cpp — color+depth RTT for the planar reflection pass.

#include "render/backends/vulkan/VkReflectionTarget.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

namespace iron {

bool VkReflectionTarget::init(VkContext& ctx, VkSampler sharedSampler) {
    sampler_ = sharedSampler;

    // --- Color image (RGBA8, USAGE_COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType = VK_IMAGE_TYPE_2D;
        iInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        iInfo.extent = {kResolution, kResolution, 1};
        iInfo.mipLevels = 1;
        iInfo.arrayLayers = 1;
        iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &colorImage_, &colorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image = colorImage_;
        vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount = 1;
        vInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &colorView_));
    }

    // --- Depth image (D32, USAGE_DEPTH_STENCIL_ATTACHMENT) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType = VK_IMAGE_TYPE_2D;
        iInfo.format = VK_FORMAT_D32_SFLOAT;
        iInfo.extent = {kResolution, kResolution, 1};
        iInfo.mipLevels = 1;
        iInfo.arrayLayers = 1;
        iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &depthImage_, &depthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image = depthImage_;
        vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format = VK_FORMAT_D32_SFLOAT;
        vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount = 1;
        vInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &depthView_));
    }

    // --- Render pass: color + depth, color finalLayout = SHADER_READ_ONLY ---
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Entry dep: EXTERNAL -> 0, FRAGMENT_SHADER -> COLOR_ATTACHMENT_OUTPUT
    // so frame N's scene-pass sampler reads of this color image complete
    // before frame N+1's reflection pass overwrites it.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // Exit dep: 0 -> EXTERNAL, COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER
    // so the scene pass can sample the color texture safely.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &renderPass_));

    // --- Framebuffer ---
    VkImageView views[2] = {colorView_, depthView_};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass_;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = views;
    fbInfo.width  = kResolution;
    fbInfo.height = kResolution;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &framebuffer_));

    return true;
}

void VkReflectionTarget::destroy(VkContext& ctx) {
    if (framebuffer_) { vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (renderPass_)  { vkDestroyRenderPass(ctx.device(), renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    if (colorView_)   { vkDestroyImageView(ctx.device(), colorView_, nullptr); colorView_ = VK_NULL_HANDLE; }
    if (depthView_)   { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (colorImage_)  { vmaDestroyImage(ctx.allocator(), colorImage_, colorAlloc_); colorImage_ = VK_NULL_HANDLE; colorAlloc_ = VK_NULL_HANDLE; }
    if (depthImage_)  { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; depthAlloc_ = VK_NULL_HANDLE; }
}

VkDescriptorImageInfo VkReflectionTarget::descriptorImageInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler     = sampler_;
    info.imageView   = colorView_;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

void VkReflectionTarget::beginPass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffer_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {kResolution, kResolution};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Same negative-height viewport convention as the scene pass so
    // GL-style projections render correctly without altering winding.
    VkViewport vp{};
    vp.x = 0;
    vp.y = static_cast<float>(kResolution);
    vp.width  = static_cast<float>(kResolution);
    vp.height = -static_cast<float>(kResolution);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {kResolution, kResolution}};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkReflectionTarget::endPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

}  // namespace iron
