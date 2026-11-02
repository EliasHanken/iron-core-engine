#include "render/backends/vulkan/VkVelocityPass.h"

#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "scene/Mesh.h"
#include "scene/SkinnedMesh.h"

#include <cstddef>
#include <cstring>

namespace iron {

namespace {

const char* const kFullscreenVert = R"(#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The scene is drawn with a flipped viewport, so clip-space y points up the
// screen while UV y points down it.
const char* const kCameraFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 outVelocity;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(push_constant) uniform Push { mat4 reprojection; } pc;
void main() {
    float depth = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 previous = pc.reprojection * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth, 1.0);
    previous /= previous.w;
    outVelocity = vUV - vec2(previous.x * 0.5 + 0.5, 0.5 - previous.y * 0.5);
}
)";

// Same transform as the lit shader (one matrix times the position), so the depth
// test against the scene lands on exactly the pixels the object covered.
const char* const kObjectVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(push_constant) uniform Push { mat4 current; mat4 previous; } pc;
layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;
void main() {
    vCurrent = pc.current * vec4(aPos, 1.0);
    vPrevious = pc.previous * vec4(aPos, 1.0);
    gl_Position = vCurrent;
}
)";

// The skinned lit shader's transform: four weighted bones, then the model's mvp.
// Slot `boneBase` holds this frame's palette, then last frame's.
const char* const kSkinnedVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(std430, set = 0, binding = 0) readonly buffer Bones { mat4 bones[256]; };
layout(push_constant) uniform Push { mat4 current; mat4 previous; } pc;
layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;
mat4 skin(int base) {
    return aWeights.x * bones[base + int(aJoints.x)] + aWeights.y * bones[base + int(aJoints.y)] +
           aWeights.z * bones[base + int(aJoints.z)] + aWeights.w * bones[base + int(aJoints.w)];
}
void main() {
    vCurrent = pc.current * (skin(0) * vec4(aPos, 1.0));
    vPrevious = pc.previous * (skin(128) * vec4(aPos, 1.0));
    gl_Position = vCurrent;
}
)";

const char* const kObjectFrag = R"(#version 450
layout(location = 0) in vec4 vCurrent;
layout(location = 1) in vec4 vPrevious;
layout(location = 0) out vec2 outVelocity;
vec2 toUv(vec4 clip) {
    vec2 ndc = clip.xy / clip.w;
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
void main() {
    outVelocity = toUv(vCurrent) - toUv(vPrevious);
}
)";

VkShaderModule makeModule(VkDevice device, VkShaderStageFlagBits stage, const char* source) {
    const auto spirv = compileGlsl(stage, source);
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(std::uint32_t);
    info.pCode    = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

}  // namespace

bool VkVelocityPass::init(VkContext& ctx, VkFormat depthFormat) {
    depthFormat_ = depthFormat;
    const VkDevice device = ctx.device();

    // Colour: written in full every frame. Depth: the opaque pass's, read only.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format         = kFormat;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format         = depthFormat;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  // Later passes read it.
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    // Entry: the opaque pass has written depth, and last frame's readers of the
    // velocity image are done with it.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // Exit: the composite and any compute can sample the motion vectors.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &pass_));

    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_NEAREST;
    sampler.minFilter    = VK_FILTER_NEAREST;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device, &sampler, nullptr, &depthSampler_));

    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout_));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_));
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &set_));

    return createPipelines(ctx);
}

bool VkVelocityPass::createPipelines(VkContext& ctx) {
    const VkDevice device = ctx.device();

    VkPushConstantRange cameraPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &cameraPush;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &cameraLayout_));

    VkPushConstantRange objectPush{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(Mat4)};
    layoutInfo.setLayoutCount      = 0;
    layoutInfo.pSetLayouts         = nullptr;
    layoutInfo.pPushConstantRanges = &objectPush;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &objectLayout_));

    {
        VkDescriptorSetLayoutBinding bones{};
        bones.binding         = 0;
        bones.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        bones.descriptorCount = 1;
        bones.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 1;
        setInfo.pBindings    = &bones;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &boneSetLayout_));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = 2;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &size;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &bonePool_));
        const VkDeviceSize block = sizeof(Mat4) * kBonesPerPalette * 2;
        for (std::size_t i = 0; i < boneBuffers_.size(); ++i) {
            BoneBuffer& b = boneBuffers_[i];
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = block * kBoneSlots;
            bi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buffer, &b.alloc, &info));
            b.mapped = info.pMappedData;
            VkDescriptorSetAllocateInfo alloc{};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = bonePool_;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &boneSetLayout_;
            VK_CHECK(vkAllocateDescriptorSets(device, &alloc, &boneSets_[i]));
            VkDescriptorBufferInfo bufferInfo{b.buffer, 0, block};
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = boneSets_[i];
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            write.pBufferInfo     = &bufferInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts    = &boneSetLayout_;
        VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinnedLayout_));
    }

    const VkShaderModule fullscreen = makeModule(device, VK_SHADER_STAGE_VERTEX_BIT, kFullscreenVert);
    const VkShaderModule camera     = makeModule(device, VK_SHADER_STAGE_FRAGMENT_BIT, kCameraFrag);
    const VkShaderModule objectVert = makeModule(device, VK_SHADER_STAGE_VERTEX_BIT, kObjectVert);
    const VkShaderModule objectFrag = makeModule(device, VK_SHADER_STAGE_FRAGMENT_BIT, kObjectFrag);
    const VkShaderModule skinnedVert = makeModule(device, VK_SHADER_STAGE_VERTEX_BIT, kSkinnedVert);
    const bool compiled = fullscreen && camera && objectVert && objectFrag && skinnedVert;

    if (compiled) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo noVertices{};
        noVertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vertexBinding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                   static_cast<std::uint32_t>(offsetof(Vertex, position))};
        VkPipelineVertexInputStateCreateInfo positions{};
        positions.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        positions.vertexBindingDescriptionCount   = 1;
        positions.pVertexBindingDescriptions      = &vertexBinding;
        positions.vertexAttributeDescriptionCount = 1;
        positions.pVertexAttributeDescriptions    = &position;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo noDepth{};
        noDepth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        // Equal-or-nearer, with no writes: a moving object only claims the pixels
        // where it is the visible surface.
        VkPipelineDepthStencilStateCreateInfo depthEqual = noDepth;
        depthEqual.depthTestEnable = VK_TRUE;
        depthEqual.depthCompareOp  = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &blend;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicInfo{};
        dynamicInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicInfo.dynamicStateCount = 2;
        dynamicInfo.pDynamicStates    = dynamicStates;

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pColorBlendState    = &colorBlend;
        info.pDynamicState       = &dynamicInfo;
        info.renderPass          = pass_;

        stages[0].module        = fullscreen;
        stages[1].module        = camera;
        info.pVertexInputState  = &noVertices;
        info.pDepthStencilState = &noDepth;
        info.layout             = cameraLayout_;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &cameraPipeline_));

        stages[0].module        = objectVert;
        stages[1].module        = objectFrag;
        info.pVertexInputState  = &positions;
        info.pDepthStencilState = &depthEqual;
        info.layout             = objectLayout_;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &objectPipeline_));

        const VkVertexInputBindingDescription skinnedBinding{0, sizeof(SkinnedVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription skinnedAttributes[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(SkinnedVertex, position))},
            {4, 0, VK_FORMAT_R32G32B32A32_UINT, static_cast<std::uint32_t>(offsetof(SkinnedVertex, joints))},
            {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(SkinnedVertex, weights))},
        };
        VkPipelineVertexInputStateCreateInfo skinnedInput{};
        skinnedInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        skinnedInput.vertexBindingDescriptionCount   = 1;
        skinnedInput.pVertexBindingDescriptions      = &skinnedBinding;
        skinnedInput.vertexAttributeDescriptionCount = 3;
        skinnedInput.pVertexAttributeDescriptions    = skinnedAttributes;
        stages[0].module       = skinnedVert;
        info.pVertexInputState = &skinnedInput;
        info.layout            = skinnedLayout_;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &skinnedPipeline_));
    } else {
        Log::error("VkVelocityPass: shader compile failed");
    }

    for (VkShaderModule module : {fullscreen, camera, objectVert, objectFrag, skinnedVert}) {
        if (module) vkDestroyShaderModule(device, module, nullptr);
    }
    return compiled;
}

bool VkVelocityPass::createTargets(VkContext& ctx, VkExtent2D extent, VkImageView depthView) {
    extent_ = extent;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = kFormat;
    imageInfo.extent        = {extent.width, extent.height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imageInfo, &allocInfo, &image_, &alloc_, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = image_;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &view_));

    const VkImageView views[2] = {view_, depthView};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = pass_;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments    = views;
    fbInfo.width           = extent.width;
    fbInfo.height          = extent.height;
    fbInfo.layers          = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &framebuffer_));

    VkDescriptorImageInfo depth{depthSampler_, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &depth;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    return true;
}

void VkVelocityPass::destroyTargets(VkContext& ctx) {
    if (framebuffer_) vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr);
    if (view_) vkDestroyImageView(ctx.device(), view_, nullptr);
    if (image_) vmaDestroyImage(ctx.allocator(), image_, alloc_);
    framebuffer_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    alloc_ = VK_NULL_HANDLE;
}

void VkVelocityPass::destroy(VkContext& ctx) {
    destroyTargets(ctx);
    const VkDevice device = ctx.device();
    if (skinnedPipeline_) vkDestroyPipeline(device, skinnedPipeline_, nullptr);
    if (skinnedLayout_) vkDestroyPipelineLayout(device, skinnedLayout_, nullptr);
    if (bonePool_) vkDestroyDescriptorPool(device, bonePool_, nullptr);
    if (boneSetLayout_) vkDestroyDescriptorSetLayout(device, boneSetLayout_, nullptr);
    for (BoneBuffer& b : boneBuffers_) {
        if (b.buffer) vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
        b = BoneBuffer{};
    }
    skinnedPipeline_ = VK_NULL_HANDLE;
    skinnedLayout_ = VK_NULL_HANDLE;
    bonePool_ = VK_NULL_HANDLE;
    boneSetLayout_ = VK_NULL_HANDLE;
    if (objectPipeline_) vkDestroyPipeline(device, objectPipeline_, nullptr);
    if (cameraPipeline_) vkDestroyPipeline(device, cameraPipeline_, nullptr);
    if (objectLayout_) vkDestroyPipelineLayout(device, objectLayout_, nullptr);
    if (cameraLayout_) vkDestroyPipelineLayout(device, cameraLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (depthSampler_) vkDestroySampler(device, depthSampler_, nullptr);
    if (pass_) vkDestroyRenderPass(device, pass_, nullptr);
    objectPipeline_ = cameraPipeline_ = VK_NULL_HANDLE;
    objectLayout_ = cameraLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    depthSampler_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
}

int VkVelocityPass::stageBones(int frame, std::span<const Mat4> current, std::span<const Mat4> previous) {
    BoneBuffer& b = boneBuffers_[static_cast<std::size_t>(frame) % boneBuffers_.size()];
    if (!b.mapped || b.used >= kBoneSlots) return -1;
    const int slot = b.used++;
    auto* out = static_cast<Mat4*>(b.mapped) + static_cast<std::size_t>(slot) * kBonesPerPalette * 2;
    const std::size_t nc = std::min<std::size_t>(current.size(), kBonesPerPalette);
    const std::size_t np = std::min<std::size_t>(previous.size(), kBonesPerPalette);
    std::memcpy(out, current.data(), nc * sizeof(Mat4));
    std::memcpy(out + kBonesPerPalette, previous.data(), np * sizeof(Mat4));
    return slot;
}

void VkVelocityPass::record(VkCommandBuffer cb, int frame, const Mat4& reprojection,
                            std::span<const VelocityObject> objects) {
    // This frame's bone slots are spent once recorded; the next use of this
    // frame's buffer starts empty (the frame ring has waited for it by then).
    BoneBuffer& bones = boneBuffers_[static_cast<std::size_t>(frame) % boneBuffers_.size()];
    const VkDescriptorSet boneSet = boneSets_[static_cast<std::size_t>(frame) % boneSets_.size()];
    struct Reset {
        BoneBuffer& b;
        ~Reset() { b.used = 0; }
    } reset{bones};
    if (!cameraPipeline_ || !objectPipeline_ || !framebuffer_) return;

    VkRenderPassBeginInfo begin{};
    begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass        = pass_;
    begin.framebuffer       = framebuffer_;
    begin.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);

    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent_.width);
    viewport.height   = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cameraPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cameraLayout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cb, cameraLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4), &reprojection);
    vkCmdDraw(cb, 3, 1, 0, 0);

    if (!objects.empty()) {
        // The scene's flipped viewport, so the objects land where they were drawn.
        viewport.y      = static_cast<float>(extent_.height);
        viewport.height = -static_cast<float>(extent_.height);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        ::VkPipeline bound = VK_NULL_HANDLE;
        for (const VelocityObject& object : objects) {
            const bool skinned = object.boneSlot >= 0 && skinnedPipeline_;
            const ::VkPipeline want = skinned ? skinnedPipeline_ : objectPipeline_;
            const VkPipelineLayout layout = skinned ? skinnedLayout_ : objectLayout_;
            if (want != bound) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                bound = want;
            }
            if (skinned) {
                const auto offset = static_cast<std::uint32_t>(sizeof(Mat4) * kBonesPerPalette * 2 *
                                                               static_cast<std::size_t>(object.boneSlot));
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedLayout_, 0, 1, &boneSet, 1,
                                        &offset);
            }
            const Mat4 matrices[2] = {object.current, object.previous};
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrices), matrices);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &object.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cb, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, object.indexCount, 1, 0, 0, 0);
        }
    }

    vkCmdEndRenderPass(cb);
}

}  // namespace iron
