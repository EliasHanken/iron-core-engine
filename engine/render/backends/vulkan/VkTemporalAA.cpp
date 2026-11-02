#include "render/backends/vulkan/VkTemporalAA.h"

#include "core/Log.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

namespace iron {

namespace {

const char* const kVert = R"(#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* const kFrag = R"GLSL(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uCurrent;   // Linear.
layout(set = 0, binding = 1) uniform sampler2D uHistory;   // Linear.
layout(set = 0, binding = 2) uniform sampler2D uVelocity;  // Nearest: UV moved since last frame.
layout(set = 0, binding = 3) uniform sampler2D uDepth;     // Nearest.
layout(set = 0, binding = 4) uniform sampler2D uOpaque;    // Nearest: the frame before transparents.
layout(push_constant) uniform Push { vec4 p; } pc;          // feedback, reset

vec3 toYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 fromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// A reversible tonemap: blending in this space keeps one bright pixel from dominating.
vec3 squash(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 unsquash(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

// Bicubic Catmull-Rom from five bilinear taps: sharp history that doesn't soften.
vec3 catmullRom(sampler2D tex, vec2 uv) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 position = uv * size;
    vec2 centre = floor(position - 0.5) + 0.5;
    vec2 f = position - centre;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (centre - 1.0) / size;
    vec2 t3 = (centre + 2.0) / size;
    vec2 t12 = (centre + w2 / w12) / size;
    vec3 sum = texture(tex, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y) +
               texture(tex, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y) +
               texture(tex, t12).rgb * (w12.x * w12.y) +
               texture(tex, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y) +
               texture(tex, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return sum / weight;
}

// Pulls `h` toward the box's centre until it's inside.
vec3 clipToBox(vec3 h, vec3 lo, vec3 hi) {
    vec3 centre = 0.5 * (lo + hi);
    vec3 extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = h - centre;
    vec3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : h;
}

void main() {
    ivec2 size = textureSize(uCurrent, 0);
    ivec2 pixel = clamp(ivec2(vUV * vec2(size)), ivec2(0), size - 1);
    vec3 current = max(texelFetch(uCurrent, pixel, 0).rgb, vec3(0.0));
    if (pc.p.y > 0.5) {
        outColor = vec4(current, 1.0);
        return;
    }

    // The neighbourhood's colour spread, and its nearest surface.
    vec3 sum = vec3(0.0), sumSq = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    float nearest = 2.0;
    ivec2 nearestPixel = pixel;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            vec3 c = toYCoCg(squash(max(texelFetch(uCurrent, q, 0).rgb, vec3(0.0))));
            sum += c;
            sumSq += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
            float d = texelFetch(uDepth, q, 0).r;
            if (d < nearest) {
                nearest = d;
                nearestPixel = q;
            }
        }
    }

    vec2 velocity = texelFetch(uVelocity, nearestPixel, 0).rg;
    vec2 previous = vUV - velocity;
    if (previous.x < 0.0 || previous.y < 0.0 || previous.x > 1.0 || previous.y > 1.0) {
        outColor = vec4(current, 1.0);  // It wasn't on screen.
        return;
    }
    vec3 history = max(catmullRom(uHistory, previous), vec3(0.0));

    // Variance clipping: a box around the mean, no wider than the neighbourhood.
    vec3 mean = sum / 9.0;
    vec3 sigma = sqrt(max(sumSq / 9.0 - mean * mean, vec3(0.0)));
    vec3 boxLo = max(lo, mean - 1.25 * sigma);
    vec3 boxHi = min(hi, mean + 1.25 * sigma);
    vec3 h = clipToBox(toYCoCg(squash(history)), boxLo, boxHi);

    // Where particles or fog changed the frame, trust the new one more.
    vec3 opaque = texelFetch(uOpaque, pixel, 0).rgb;
    float reactive = clamp(length(current - opaque) / (1.0 + dot(current, vec3(0.3, 0.6, 0.1))) * 4.0, 0.0, 1.0);
    // Fast motion keeps less history, so it doesn't smear.
    float speed = length(velocity * vec2(size));
    float feedback = pc.p.x;
    feedback = mix(feedback, 0.55, reactive);
    feedback *= mix(1.0, 0.85, clamp(speed / 40.0, 0.0, 1.0));

    vec3 blended = mix(squash(current), fromYCoCg(h), feedback);
    outColor = vec4(max(unsquash(blended), vec3(0.0)), 1.0);
}
)GLSL";

// Contrast-adaptive sharpening, after AMD's FidelityFX CAS: each pixel is pushed
// away from its four neighbours' average, less where the neighbourhood is already
// near black or white, so edges crisp up without ringing. Works in the same
// reversible tonemapped space as the resolve, since the scene is HDR.
const char* const kSharpenFrag = R"GLSL(#version 450
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uImage;
layout(push_constant) uniform Push { vec4 p; } pc;  // x = sharpness 0..1
vec3 squash(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 unsquash(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
vec3 at(ivec2 p, ivec2 size) { return squash(max(texelFetch(uImage, clamp(p, ivec2(0), size - 1), 0).rgb, 0.0)); }
void main() {
    ivec2 size = textureSize(uImage, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 b = at(p + ivec2(0, -1), size), d = at(p + ivec2(-1, 0), size), e = at(p, size);
    vec3 f = at(p + ivec2(1, 0), size), h = at(p + ivec2(0, 1), size);
    vec3 lo = min(min(min(b, d), min(e, f)), h);
    vec3 hi = max(max(max(b, d), max(e, f)), h);
    vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
    vec3 w = -amp * mix(0.04, 0.2, clamp(pc.p.x, 0.0, 1.0));
    vec3 o = (e + (b + d + f + h) * w) / (1.0 + 4.0 * w);
    outColor = vec4(unsquash(clamp(o, 0.0, 0.999)), 1.0);
}
)GLSL";

VkShaderModule moduleFor(VkDevice device, VkShaderStageFlagBits stage, const char* source) {
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

VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                                  VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask               = src;
    b.dstAccessMask               = dst;
    b.oldLayout                   = from;
    b.newLayout                   = to;
    b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.image                       = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    return b;
}

void copyImage(VkCommandBuffer cb, VkImage from, VkImage to, VkExtent2D extent) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent         = {extent.width, extent.height, 1};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
}

}  // namespace

bool VkTemporalAA::init(VkContext& ctx, VkFormat colorFormat) {
    format_ = colorFormat;
    const VkDevice device = ctx.device();

    // Every texel is written, so nothing needs loading; the result is copied out.
    VkAttachmentDescription color{};
    color.format         = colorFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &ref;
    VkSubpassDependency deps[2]{};
    // Entry: last frame's reads of this history, and this frame's scene, velocity and
    // depth writes, are done.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    // Exit: the sharpen pass samples it.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &color;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &subpass;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &pass_));

    // The sharpen pass writes the scene image whole. Entry: the resolve has read it
    // and written the history it samples. Exit: bloom and the composite read it.
    {
        VkAttachmentDescription scene = color;
        scene.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        scene.finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkSubpassDependency sdeps[2]{};
        sdeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        sdeps[0].dstSubpass    = 0;
        sdeps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        sdeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        sdeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sdeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        sdeps[1].srcSubpass    = 0;
        sdeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        sdeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        sdeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT;
        sdeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sdeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo srp = rp;
        srp.pAttachments  = &scene;
        srp.pDependencies = sdeps;
        VK_CHECK(vkCreateRenderPass(device, &srp, nullptr, &sharpenPass_));
    }

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device, &si, nullptr, &linear_));
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(device, &si, nullptr, &nearest_));

    VkDescriptorSetLayoutBinding b[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout_));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &pi, nullptr, &pool_));
    const VkDescriptorSetLayout layouts[2] = {setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(device, &ai, sets_.data()));

    {
        VkDescriptorSetLayoutBinding one{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = 1;
        sli.pBindings    = &one;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &sli, nullptr, &sharpenSetLayout_));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo spi{};
        spi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spi.maxSets       = 2;
        spi.poolSizeCount = 1;
        spi.pPoolSizes    = &size;
        VK_CHECK(vkCreateDescriptorPool(device, &spi, nullptr, &sharpenPool_));
        const VkDescriptorSetLayout sl[2] = {sharpenSetLayout_, sharpenSetLayout_};
        VkDescriptorSetAllocateInfo sai{};
        sai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sai.descriptorPool     = sharpenPool_;
        sai.descriptorSetCount = 2;
        sai.pSetLayouts        = sl;
        VK_CHECK(vkAllocateDescriptorSets(device, &sai, sharpenSets_.data()));
    }

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &layout_));
    pl.pSetLayouts = &sharpenSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &sharpenLayout_));

    const VkShaderModule vert = moduleFor(device, VK_SHADER_STAGE_VERTEX_BIT, kVert);
    const VkShaderModule frag = moduleFor(device, VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    const VkShaderModule sharpen = moduleFor(device, VK_SHADER_STAGE_FRAGMENT_BIT, kSharpenFrag);
    if (!vert || !frag || !sharpen) {
        Log::error("VkTemporalAA: shader compile failed");
        for (VkShaderModule m : {vert, frag, sharpen}) {
            if (m) vkDestroyShaderModule(device, m, nullptr);
        }
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert,
                 "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag,
                 "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
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
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &colorBlend;
    info.pDynamicState       = &dynamicInfo;
    info.layout              = layout_;
    info.renderPass          = pass_;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_));
    stages[1].module = sharpen;
    info.layout      = sharpenLayout_;
    info.renderPass  = sharpenPass_;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &sharpenPipeline_));
    for (VkShaderModule m : {vert, frag, sharpen}) vkDestroyShaderModule(device, m, nullptr);
    return pipeline_ != VK_NULL_HANDLE && sharpenPipeline_ != VK_NULL_HANDLE;
}

VkTemporalAA::Image VkTemporalAA::makeImage(VkContext& ctx, VkImageUsageFlags usage) const {
    Image out;
    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = format_;
    ii.extent        = {extent_.width, extent_.height, 1};
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ii, &ai, &out.image, &out.alloc, nullptr));
    VkImageViewCreateInfo vi{};
    vi.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                       = out.image;
    vi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    vi.format                      = format_;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &out.view));
    return out;
}

void VkTemporalAA::freeImage(VkContext& ctx, Image& image) const {
    if (image.view) vkDestroyImageView(ctx.device(), image.view, nullptr);
    if (image.image) vmaDestroyImage(ctx.allocator(), image.image, image.alloc);
    image = Image{};
}

bool VkTemporalAA::createTargets(VkContext& ctx, VkExtent2D extent, VkImage sceneColor, VkImageView sceneColorView,
                                 VkImageView depthView, VkImageView velocityView) {
    extent_ = extent;
    sceneColor_ = sceneColor;
    const VkImageUsageFlags historyUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (std::size_t i = 0; i < 2; ++i) {
        history_[i] = makeImage(ctx, historyUsage);
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = pass_;
        fb.attachmentCount = 1;
        fb.pAttachments    = &history_[i].view;
        fb.width           = extent.width;
        fb.height          = extent.height;
        fb.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fb, nullptr, &framebuffers_[i]));
    }
    opaque_ = makeImage(ctx, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = sharpenPass_;
        fb.attachmentCount = 1;
        fb.pAttachments    = &sceneColorView;
        fb.width           = extent.width;
        fb.height          = extent.height;
        fb.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fb, nullptr, &sceneFramebuffer_));
    }

    // Into the layouts the first frame expects: histories and the copy are sampled.
    {
        VkCommandPoolCreateInfo cp{};
        cp.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.queueFamilyIndex = ctx.graphicsFamily();
        cp.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &cp, nullptr, &pool));
        VkCommandBufferAllocateInfo cba{};
        cba.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cba.commandPool        = pool;
        cba.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cba.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cba, &cb));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &begin));
        VkImageMemoryBarrier barriers[3] = {
            imageBarrier(history_[0].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                         VK_ACCESS_SHADER_READ_BIT),
            imageBarrier(history_[1].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                         VK_ACCESS_SHADER_READ_BIT),
            imageBarrier(opaque_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                         VK_ACCESS_SHADER_READ_BIT)};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 3, barriers);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
    }

    for (std::size_t i = 0; i < 2; ++i) {
        const VkDescriptorImageInfo images[5] = {
            {linear_, sceneColorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {linear_, history_[1 - i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_, velocityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {nearest_, opaque_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet writes[5]{};
        for (std::uint32_t b = 0; b < 5; ++b) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = sets_[i];
            writes[b].dstBinding      = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo      = &images[b];
        }
        vkUpdateDescriptorSets(ctx.device(), 5, writes, 0, nullptr);

        const VkDescriptorImageInfo history{nearest_, history_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet sharpenWrite{};
        sharpenWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sharpenWrite.dstSet          = sharpenSets_[i];
        sharpenWrite.dstBinding      = 0;
        sharpenWrite.descriptorCount = 1;
        sharpenWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sharpenWrite.pImageInfo      = &history;
        vkUpdateDescriptorSets(ctx.device(), 1, &sharpenWrite, 0, nullptr);
    }
    reset_ = true;
    return true;
}

void VkTemporalAA::destroyTargets(VkContext& ctx) {
    if (sceneFramebuffer_) vkDestroyFramebuffer(ctx.device(), sceneFramebuffer_, nullptr);
    sceneFramebuffer_ = VK_NULL_HANDLE;
    for (auto& fb : framebuffers_) {
        if (fb) vkDestroyFramebuffer(ctx.device(), fb, nullptr);
        fb = VK_NULL_HANDLE;
    }
    for (auto& h : history_) freeImage(ctx, h);
    freeImage(ctx, opaque_);
}

void VkTemporalAA::destroy(VkContext& ctx) {
    destroyTargets(ctx);
    const VkDevice device = ctx.device();
    if (sharpenPipeline_) vkDestroyPipeline(device, sharpenPipeline_, nullptr);
    if (sharpenLayout_) vkDestroyPipelineLayout(device, sharpenLayout_, nullptr);
    if (sharpenPool_) vkDestroyDescriptorPool(device, sharpenPool_, nullptr);
    if (sharpenSetLayout_) vkDestroyDescriptorSetLayout(device, sharpenSetLayout_, nullptr);
    if (sharpenPass_) vkDestroyRenderPass(device, sharpenPass_, nullptr);
    sharpenPipeline_ = VK_NULL_HANDLE;
    sharpenLayout_ = VK_NULL_HANDLE;
    sharpenPool_ = VK_NULL_HANDLE;
    sharpenSetLayout_ = VK_NULL_HANDLE;
    sharpenPass_ = VK_NULL_HANDLE;
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (linear_) vkDestroySampler(device, linear_, nullptr);
    if (nearest_) vkDestroySampler(device, nearest_, nullptr);
    if (pass_) vkDestroyRenderPass(device, pass_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
}

void VkTemporalAA::captureOpaque(VkCommandBuffer cb) const {
    if (!opaque_.image) return;
    VkImageMemoryBarrier before[2] = {
        imageBarrier(sceneColor_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        imageBarrier(opaque_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
    copyImage(cb, sceneColor_, opaque_.image, extent_);
    VkImageMemoryBarrier after[2] = {
        imageBarrier(sceneColor_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT),
        imageBarrier(opaque_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, after);
}

void VkTemporalAA::resolve(VkCommandBuffer cb) {
    if (!pipeline_ || !framebuffers_[0]) return;
    current_ = 1 - current_;
    const auto i = static_cast<std::size_t>(current_);

    VkRenderPassBeginInfo begin{};
    begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass        = pass_;
    begin.framebuffer       = framebuffers_[i];
    begin.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[i], 0, nullptr);
    const float push[4] = {0.9f, reset_ ? 1.0f : 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    reset_ = false;

    // Back into the scene image, sharpened, so the rest of the frame reads the result.
    begin.renderPass  = sharpenPass_;
    begin.framebuffer = sceneFramebuffer_;
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sharpenPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sharpenLayout_, 0, 1, &sharpenSets_[i], 0, nullptr);
    const float sharpen[4] = {sharpness_, 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cb, sharpenLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sharpen), sharpen);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
}

}  // namespace iron
