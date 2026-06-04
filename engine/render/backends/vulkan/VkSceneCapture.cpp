// VkSceneCapture.cpp — on-demand 6-face scene capture into an RGBA16F cube.
//
// Renders the scene from a world position down each of the 6 cube faces into a
// COLOR_ATTACHMENT+SAMPLED cube (VkCubemapStore::createColorCube), with simple
// sun+ambient+diffuse shading. When a valid skyboxCube is supplied the HDR sky
// is drawn into each face FIRST (as background) using a dedicated skybox
// pipeline (mirrors VkSkybox — gl_Position.xyww + depthWrite OFF +
// LESS_OR_EQUAL, translation-stripped viewProj), then the scene geometry on
// top. Runs OFF the per-frame path (blocking submit + queue-wait-idle, à la
// VkIblBaker), so it owns ALL its resources and never touches VkFrameRing.
// The resulting cube is later fed to bakePrefiltered.

#include "render/backends/vulkan/VkSceneCapture.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/ReflectionProbe.h"   // cubeFaceView
#include "math/Mat4.h"
#include "math/Transform.h"           // perspective
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

// std140-compatible per-draw uniform. mat4 + mat4 + 4×vec4 = 64+64+64 = 192.
struct CapUbo {
    Mat4 mvp;       // faceViewProj * model
    Mat4 model;
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 baseColorFactor;  // xyz = material albedo tint (untextured materials rely on this)
};
static_assert(sizeof(CapUbo) == 192, "CapUbo must be 192 bytes (std140)");

// 256-byte alignment for dynamic UBO offsets — safely >= any device's
// minUniformBufferOffsetAlignment we target. (VkContext exposes no accessor.)
// Budget is cumulative across all 6 faces: up to kMaxDraws per face => kMaxSets total
// geometry descriptor sets and kMaxSets*kUboStride geometry UBO bytes, plus kMaxFaces
// extra sets and UBO slices for the per-face skybox draw.
constexpr VkDeviceSize kUboStride   = 256;
constexpr std::uint32_t kMaxFaces   = 6;
constexpr std::uint32_t kMaxDraws   = 512;          // per face, before we warn+cap
constexpr std::uint32_t kMaxSets    = kMaxFaces * kMaxDraws;
// Total UBO: kMaxSets geometry slices + kMaxFaces skybox slices.
constexpr VkDeviceSize kUboBytes    = static_cast<VkDeviceSize>(kMaxSets + kMaxFaces) * kUboStride;

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

const char* kCaptureVert = R"(#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUv;
layout(location=3) in vec3 inTangent;
layout(set=0, binding=0) uniform CapUbo {
    mat4 mvp;
    mat4 model;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 baseColorFactor;
} u;
layout(location=0) out vec3 vN;
layout(location=1) out vec2 vUv;
void main() {
    gl_Position = u.mvp * vec4(inPos, 1.0);
    vN  = mat3(u.model) * inNormal;
    vUv = inUv;
}
)";

const char* kCaptureFrag = R"(#version 450
layout(set=0, binding=1) uniform sampler2D uDiffuse;
layout(set=0, binding=0) uniform CapUbo {
    mat4 mvp; mat4 model; vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 baseColorFactor;
} u;
layout(location=0) in vec3 vN;
layout(location=1) in vec2 vUv;
layout(location=0) out vec4 outColor;
void main() {
    vec3 N = normalize(vN);
    float ndl = max(dot(N, -normalize(u.sunDir.xyz)), 0.0);
    // Untextured materials carry their color in baseColorFactor; uDiffuse is the
    // white fallback for them, so albedo must be the product (matches the lit shader).
    vec3 albedo = u.baseColorFactor.rgb * texture(uDiffuse, vUv).rgb;
    outColor = vec4(albedo * (u.ambient.xyz + u.sunColor.xyz * ndl), 1.0);
}
)";

// Skybox shaders — mirror VkSkybox exactly (same UBO layout, same
// gl_Position.xyww trick, same samplerCube lookup).
const char* kSkyVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(set = 0, binding = 0) uniform SkyUbo {
    mat4 viewProjection;
} u;
layout(location = 0) out vec3 vDir;
void main() {
    vDir = aPos;
    vec4 pos = u.viewProjection * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

const char* kSkyFrag = R"(#version 450
layout(location = 0) in vec3 vDir;
layout(set = 0, binding = 1) uniform samplerCube uSkyCubemap;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(texture(uSkyCubemap, vDir).rgb, 1.0);
}
)";

// 8 unique cube corners, 36 indices (matches VkSkybox).
constexpr float kCubeVerts[24] = {
    -1.0f, -1.0f, -1.0f,  // 0
     1.0f, -1.0f, -1.0f,  // 1
     1.0f,  1.0f, -1.0f,  // 2
    -1.0f,  1.0f, -1.0f,  // 3
    -1.0f, -1.0f,  1.0f,  // 4
     1.0f, -1.0f,  1.0f,  // 5
     1.0f,  1.0f,  1.0f,  // 6
    -1.0f,  1.0f,  1.0f,  // 7
};
constexpr std::uint32_t kCubeIdx[36] = {
    // -Z face
    0, 2, 1,   0, 3, 2,
    // +Z face
    4, 5, 6,   4, 6, 7,
    // -X face
    0, 4, 7,   0, 7, 3,
    // +X face
    1, 2, 6,   1, 6, 5,
    // -Y face
    0, 1, 5,   0, 5, 4,
    // +Y face
    3, 7, 6,   3, 6, 2,
};

// Per-face skybox UBO: just the translation-stripped viewProj (matches SkyUbo in VkSkybox).
struct SkyUbo { float viewProjection[16]; };

}  // namespace

bool VkSceneCapture::init(VkContext& ctx) {
    // --- Render pass: 1 color (RGBA16F, finalLayout SHADER_READ) + depth ---
    // Mirrors VkReflectionTarget: entry+exit subpass dependencies so the cube
    // is safe to sample after the capture (and re-bakeable on the next call).
    VkAttachmentDescription attachments[2]{};
    attachments[0].format         = kColorFormat;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format         = kDepthFormat;
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
    // Entry: any prior sampler reads of a previous face complete before we
    // start writing this face's color.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // Exit: color writes complete before the cube is sampled (prefilter / lit).
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
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &renderPass_));

    // --- Descriptor set layout: UBO@0 (vert+frag) + sampler@1 (frag) ---
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding         = 1;
    b[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 2;
    sli.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &sli, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &pipelineLayout_));

    // --- Shaders ---
    auto vertSpv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kCaptureVert);
    if (vertSpv.empty()) { Log::error("VkSceneCapture: vert SPIR-V compile failed"); return false; }
    auto fragSpv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCaptureFrag);
    if (fragSpv.empty()) { Log::error("VkSceneCapture: frag SPIR-V compile failed"); return false; }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertSpv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vertSpv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vert_));
    smInfo.codeSize = fragSpv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fragSpv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &frag_));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_;
    stages[1].pName  = "main";

    // Vertex input — match VkMesh layout (Pos, Normal, UV, Tangent = 11 floats).
    // must match VkMesh vertex layout: Pos(3)+Normal(3)+UV(2)+Tangent(3) = 11 floats
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = 11 * sizeof(float);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    6 * sizeof(float)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 * sizeof(float)};

    VkPipelineVertexInputStateCreateInfo vinfo{};
    vinfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vinfo.vertexBindingDescriptionCount   = 1;
    vinfo.pVertexBindingDescriptions      = &bind;
    vinfo.vertexAttributeDescriptionCount = 4;
    vinfo.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo iaInfo{};
    iaInfo.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpInfo{};
    vpInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpInfo.viewportCount = 1;
    vpInfo.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;  // capture all geometry from inside the scene
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vinfo;
    gp.pInputAssemblyState = &iaInfo;
    gp.pViewportState      = &vpInfo;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dynInfo;
    gp.layout              = pipelineLayout_;
    gp.renderPass          = renderPass_;
    gp.subpass             = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &gp,
                                       nullptr, &pipeline_));

    // --- Skybox pipeline ---
    // DSL: binding 0 = UBO (vert), binding 1 = samplerCube (frag).
    {
        VkDescriptorSetLayoutBinding skyBindings[2]{};
        skyBindings[0].binding         = 0;
        skyBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        skyBindings[0].descriptorCount = 1;
        skyBindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        skyBindings[1].binding         = 1;
        skyBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skyBindings[1].descriptorCount = 1;
        skyBindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo skyDslInfo{};
        skyDslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        skyDslInfo.bindingCount = 2;
        skyDslInfo.pBindings    = skyBindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &skyDslInfo, nullptr, &skySetLayout_));

        VkPipelineLayoutCreateInfo skyPli{};
        skyPli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        skyPli.setLayoutCount = 1;
        skyPli.pSetLayouts    = &skySetLayout_;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &skyPli, nullptr, &skyPipelineLayout_));

        // Compile skybox shaders.
        auto skyVertSpv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kSkyVert);
        if (skyVertSpv.empty()) { Log::error("VkSceneCapture: sky vert SPIR-V compile failed"); return false; }
        auto skyFragSpv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kSkyFrag);
        if (skyFragSpv.empty()) { Log::error("VkSceneCapture: sky frag SPIR-V compile failed"); return false; }

        VkShaderModule skyVert = VK_NULL_HANDLE, skyFrag = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo skySm{};
        skySm.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        skySm.codeSize = skyVertSpv.size() * sizeof(std::uint32_t);
        skySm.pCode    = skyVertSpv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &skySm, nullptr, &skyVert));
        skySm.codeSize = skyFragSpv.size() * sizeof(std::uint32_t);
        skySm.pCode    = skyFragSpv.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &skySm, nullptr, &skyFrag));

        VkPipelineShaderStageCreateInfo skyStages[2]{};
        skyStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        skyStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        skyStages[0].module = skyVert;
        skyStages[0].pName  = "main";
        skyStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        skyStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        skyStages[1].module = skyFrag;
        skyStages[1].pName  = "main";

        // Vertex input: only position (3 floats), stride 12.
        VkVertexInputBindingDescription skyBind{};
        skyBind.binding   = 0;
        skyBind.stride    = sizeof(float) * 3;
        skyBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription skyAttr{};
        skyAttr.location = 0; skyAttr.binding = 0;
        skyAttr.format   = VK_FORMAT_R32G32B32_SFLOAT;
        skyAttr.offset   = 0;

        VkPipelineVertexInputStateCreateInfo skyVi{};
        skyVi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        skyVi.vertexBindingDescriptionCount   = 1;
        skyVi.pVertexBindingDescriptions      = &skyBind;
        skyVi.vertexAttributeDescriptionCount = 1;
        skyVi.pVertexAttributeDescriptions    = &skyAttr;

        VkPipelineInputAssemblyStateCreateInfo skyIa{};
        skyIa.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        skyIa.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo skyVpState{};
        skyVpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        skyVpState.viewportCount = 1;
        skyVpState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo skyRs{};
        skyRs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        skyRs.polygonMode = VK_POLYGON_MODE_FILL;
        skyRs.cullMode    = VK_CULL_MODE_NONE;
        skyRs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        skyRs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo skyMs{};
        skyMs.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        skyMs.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth: test ON (skybox drawn before geometry), write OFF, LESS_OR_EQUAL
        // so the z=1 gl_Position.xyww trick works correctly.
        VkPipelineDepthStencilStateCreateInfo skyDs{};
        skyDs.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        skyDs.depthTestEnable  = VK_TRUE;
        skyDs.depthWriteEnable = VK_FALSE;
        skyDs.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState skyCba{};
        skyCba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo skyCb{};
        skyCb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        skyCb.attachmentCount = 1;
        skyCb.pAttachments    = &skyCba;

        VkDynamicState skyDynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo skyDyn{};
        skyDyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        skyDyn.dynamicStateCount = 2;
        skyDyn.pDynamicStates    = skyDynStates;

        VkGraphicsPipelineCreateInfo skyGp{};
        skyGp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        skyGp.stageCount          = 2;
        skyGp.pStages             = skyStages;
        skyGp.pVertexInputState   = &skyVi;
        skyGp.pInputAssemblyState = &skyIa;
        skyGp.pViewportState      = &skyVpState;
        skyGp.pRasterizationState = &skyRs;
        skyGp.pMultisampleState   = &skyMs;
        skyGp.pDepthStencilState  = &skyDs;
        skyGp.pColorBlendState    = &skyCb;
        skyGp.pDynamicState       = &skyDyn;
        skyGp.layout              = skyPipelineLayout_;
        skyGp.renderPass          = renderPass_;
        skyGp.subpass             = 0;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &skyGp,
                                           nullptr, &skyPipeline_));

        vkDestroyShaderModule(ctx.device(), skyVert, nullptr);
        vkDestroyShaderModule(ctx.device(), skyFrag, nullptr);

        // Upload skybox cube geometry to device-local buffers via staging.
        auto uploadDeviceLocal = [&](const void* src, VkDeviceSize size,
                                     VkBufferUsageFlags usage,
                                     VkBuffer& outBuf, VmaAllocation& outAlloc) {
            VkBufferCreateInfo dbi{};
            dbi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            dbi.size        = size;
            dbi.usage       = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            dbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo dai{};
            dai.usage = VMA_MEMORY_USAGE_AUTO;
            VK_CHECK(vmaCreateBuffer(ctx.allocator(), &dbi, &dai, &outBuf, &outAlloc, nullptr));

            // Staging buffer.
            VkBufferCreateInfo sbi = dbi;
            sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo sai{};
            sai.usage = VMA_MEMORY_USAGE_AUTO;
            sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo sallocInfo{};
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;
            VK_CHECK(vmaCreateBuffer(ctx.allocator(), &sbi, &sai,
                                     &staging, &stagingAlloc, &sallocInfo));
            std::memcpy(sallocInfo.pMappedData, src, size);
            vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, size);

            VkCommandPoolCreateInfo spi{};
            spi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            spi.queueFamilyIndex = ctx.graphicsFamily();
            spi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            VkCommandPool spool = VK_NULL_HANDLE;
            VK_CHECK(vkCreateCommandPool(ctx.device(), &spi, nullptr, &spool));
            VkCommandBufferAllocateInfo scbi{};
            scbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            scbi.commandPool        = spool;
            scbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            scbi.commandBufferCount = 1;
            VkCommandBuffer scb = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &scbi, &scb));
            VkCommandBufferBeginInfo sbb{};
            sbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            sbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(scb, &sbb));
            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(scb, staging, outBuf, 1, &copy);
            VK_CHECK(vkEndCommandBuffer(scb));
            VkSubmitInfo ss{};
            ss.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            ss.commandBufferCount = 1;
            ss.pCommandBuffers    = &scb;
            VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &ss, VK_NULL_HANDLE));
            VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
            vkDestroyCommandPool(ctx.device(), spool, nullptr);
            vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
        };

        uploadDeviceLocal(kCubeVerts, sizeof(kCubeVerts),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          skyVertexBuffer_, skyVertexAlloc_);
        uploadDeviceLocal(kCubeIdx, sizeof(kCubeIdx),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          skyIndexBuffer_, skyIndexAlloc_);
    }

    // --- Descriptor pool (reset+reused each capture) ---
    // Budget: kMaxSets for geometry draws + kMaxFaces extra for the skybox draws.
    const std::uint32_t totalSets = kMaxSets + kMaxFaces;
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = totalSets;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = totalSets;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = totalSets;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &descPool_));

    // --- Host-visible UBO linear allocator ---
    // Budget: kMaxSets geometry UBO slices + kMaxFaces skybox UBO slices.
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = kUboBytes;
    bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo aiOut{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &uboBuffer_, &uboAlloc_, &aiOut));
    uboMapped_ = aiOut.pMappedData;

    return true;
}

bool VkSceneCapture::ensureDepth(VkContext& ctx, std::uint32_t faceSize) {
    if (depthImage_ != VK_NULL_HANDLE && faceSize <= depthSize_) return true;

    // Recreate at the larger size.
    if (depthView_)  { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_) { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; depthAlloc_ = VK_NULL_HANDLE; }

    VkImageCreateInfo iInfo{};
    iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iInfo.imageType     = VK_IMAGE_TYPE_2D;
    iInfo.format        = kDepthFormat;
    iInfo.extent        = {faceSize, faceSize, 1};
    iInfo.mipLevels     = 1;
    iInfo.arrayLayers   = 1;
    iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aInfo{};
    aInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo, &depthImage_, &depthAlloc_, nullptr));

    VkImageViewCreateInfo vInfo{};
    vInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vInfo.image                       = depthImage_;
    vInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    vInfo.format                      = kDepthFormat;
    vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vInfo.subresourceRange.levelCount = 1;
    vInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &depthView_));

    depthSize_ = faceSize;
    return true;
}

CubemapHandle VkSceneCapture::capture(VkContext& ctx, VkCubemapStore& cubes,
                                      VkMeshStore& meshes, VkTextureStore& textures,
                                      const std::vector<DrawCall>& sceneDraws,
                                      Vec3 sunDir, Vec3 sunColor, Vec3 ambient,
                                      Vec3 position, int faceSize,
                                      CubemapHandle skyboxCube) {
    if (faceSize <= 0) {
        Log::error("VkSceneCapture::capture: invalid faceSize %d", faceSize);
        return kInvalidHandle;
    }

    const std::uint32_t fs = static_cast<std::uint32_t>(faceSize);

    // 1. Destination cube.
    const CubemapHandle cube = cubes.createColorCube(ctx, faceSize);
    if (cube == kInvalidHandle) {
        Log::error("VkSceneCapture: createColorCube failed");
        return kInvalidHandle;
    }

    // 2. Depth (re)creation if needed.
    if (!ensureDepth(ctx, fs)) {
        cubes.destroy(ctx, cube);
        return kInvalidHandle;
    }

    // 3. Reset per-capture transient state.
    VK_CHECK(vkResetDescriptorPool(ctx.device(), descPool_, 0));
    VkDeviceSize uboOffset = 0;

    // 4. One-shot command pool + cb (VkIblBaker pattern).
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

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    const Mat4 proj = perspective(1.5707963f /*90deg*/, 1.0f, 0.05f, 1000.0f);
    const VkCubemapResource& cubeRes = cubes.get(cube);

    std::vector<VkFramebuffer> framebuffers;  // kept alive until after waitIdle
    framebuffers.reserve(kMaxFaces);

    std::uint32_t drawCap = static_cast<std::uint32_t>(sceneDraws.size());
    if (drawCap > kMaxDraws) {
        Log::warn("VkSceneCapture: %u draws exceeds per-face capacity %u; capping",
                  drawCap, kMaxDraws);
        drawCap = kMaxDraws;
    }

    for (int face = 0; face < static_cast<int>(kMaxFaces); ++face) {
        // Transient framebuffer: this face's color view + shared depth.
        VkImageView views[2] = {cubeRes.faceViews[face], depthView_};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = renderPass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = views;
        fbInfo.width           = fs;
        fbInfo.height          = fs;
        fbInfo.layers          = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &fb));
        framebuffers.push_back(fb);

        VkClearValue clears[2]{};
        clears[0].color.float32[0] = 0.5f;
        clears[0].color.float32[1] = 0.6f;
        clears[0].color.float32[2] = 0.7f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = renderPass_;
        rpBegin.framebuffer       = fb;
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = {fs, fs};
        rpBegin.clearValueCount   = 2;
        rpBegin.pClearValues      = clears;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        // Positive-height viewport: unlike the on-screen scene pass, this cube is
        // sampled by direction (samplerCube), so it must match the cubeFaceDirection
        // texel convention — NOT the screen-presentation flip. A negative-height
        // viewport flips every face's screen-Y, which flips the side faces' sky/
        // ground gradient (upside-down) and mismatches them against the ±Y faces.
        VkViewport vp{};
        vp.x        = 0;
        vp.y        = 0;
        vp.width    = static_cast<float>(fs);
        vp.height   = static_cast<float>(fs);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, {fs, fs}};
        vkCmdSetScissor(cb, 0, 1, &scissor);

        const Mat4 faceView = cubeFaceView(position, face);
        const Mat4 faceVP   = proj * faceView;

        // --- Skybox draw (before geometry) ---
        // Draw the HDR skybox as background when a valid skybox cubemap is set.
        // Uses a translation-stripped viewProj: proj * mat4(mat3(faceView)), i.e.
        // zero the 4th column's translation entries so the sky appears infinitely
        // far. This mirrors exactly how VkSkybox's caller strips translation
        // (mat4(mat3(view)) in GLSL, or equivalently zeroing col 3 rows 0..2 in C++).
        if (skyboxCube != kInvalidHandle && cubes.has(skyboxCube)) {
            // Build the translation-stripped viewProj: keep only the rotation part
            // of faceView (zero the translation in column 3, rows 0..2).
            Mat4 skyView = faceView;
            skyView.at(0, 3) = 0.0f;
            skyView.at(1, 3) = 0.0f;
            skyView.at(2, 3) = 0.0f;
            const Mat4 skyVP = proj * skyView;

            SkyUbo skyUbo;
            std::memcpy(skyUbo.viewProjection, skyVP.m, sizeof(skyUbo.viewProjection));
            std::memcpy(static_cast<char*>(uboMapped_) + uboOffset, &skyUbo, sizeof(SkyUbo));
            const VkDeviceSize skyUboOffset = uboOffset;
            uboOffset += kUboStride;

            VkDescriptorSetAllocateInfo skyDsInfo{};
            skyDsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            skyDsInfo.descriptorPool     = descPool_;
            skyDsInfo.descriptorSetCount = 1;
            skyDsInfo.pSetLayouts        = &skySetLayout_;
            VkDescriptorSet skySet = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &skyDsInfo, &skySet));

            VkDescriptorBufferInfo skyBufInfo{};
            skyBufInfo.buffer = uboBuffer_;
            skyBufInfo.offset = skyUboOffset;
            skyBufInfo.range  = sizeof(SkyUbo);

            const VkCubemapResource& skyRes = cubes.get(skyboxCube);
            VkDescriptorImageInfo skyImgInfo{};
            skyImgInfo.sampler     = skyRes.sampler;
            skyImgInfo.imageView   = skyRes.view;
            skyImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet skyWrites[2]{};
            skyWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skyWrites[0].dstSet          = skySet;
            skyWrites[0].dstBinding      = 0;
            skyWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            skyWrites[0].descriptorCount = 1;
            skyWrites[0].pBufferInfo     = &skyBufInfo;
            skyWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skyWrites[1].dstSet          = skySet;
            skyWrites[1].dstBinding      = 1;
            skyWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            skyWrites[1].descriptorCount = 1;
            skyWrites[1].pImageInfo      = &skyImgInfo;
            vkUpdateDescriptorSets(ctx.device(), 2, skyWrites, 0, nullptr);

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    skyPipelineLayout_, 0, 1, &skySet, 0, nullptr);
            VkDeviceSize skyVboOffsets[1] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, &skyVertexBuffer_, skyVboOffsets);
            vkCmdBindIndexBuffer(cb, skyIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
        }

        // --- Geometry draws ---
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        std::uint32_t drawn = 0;
        for (const DrawCall& call : sceneDraws) {
            if (drawn >= drawCap) break;
            if (call.material.useReflectionPlane) continue;  // skip planar reflectors
            if (call.material.excludeFromProbeCapture) continue;  // M49: don't capture chrome/probe markers
            if (!meshes.has(call.mesh)) continue;
            ++drawn;

            // Write the per-draw UBO at the current linear offset.
            CapUbo ubo{};
            ubo.mvp      = faceVP * call.model;
            ubo.model    = call.model;
            ubo.sunDir   = Vec4{sunDir.x, sunDir.y, sunDir.z, 0.0f};
            ubo.sunColor = Vec4{sunColor.x, sunColor.y, sunColor.z, 0.0f};
            ubo.ambient  = Vec4{ambient.x, ambient.y, ambient.z, 0.0f};
            ubo.baseColorFactor = Vec4{call.material.baseColorFactor.x,
                                       call.material.baseColorFactor.y,
                                       call.material.baseColorFactor.z, 1.0f};
            std::memcpy(static_cast<char*>(uboMapped_) + uboOffset, &ubo, sizeof(CapUbo));
            const VkDeviceSize thisOffset = uboOffset;
            uboOffset += kUboStride;

            // Allocate + write a descriptor set.
            VkDescriptorSetAllocateInfo dsInfo{};
            dsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsInfo.descriptorPool     = descPool_;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts        = &setLayout_;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = uboBuffer_;
            bufInfo.offset = thisOffset;
            bufInfo.range  = sizeof(CapUbo);

            const VkTextureResource& diff =
                textures.has(call.material.texture)
                    ? textures.get(call.material.texture)
                    : textures.get(textures.whiteTexture());
            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler     = diff.sampler;
            imgInfo.imageView   = diff.view;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = set;
            writes[0].dstBinding      = 0;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo     = &bufInfo;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = set;
            writes[1].dstBinding      = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo      = &imgInfo;
            vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 0, 1, &set, 0, nullptr);

            const VkMeshResource& mesh = meshes.get(call.mesh);
            VkDeviceSize offsets[1] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
            vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cb);
    }

    // Flush the written UBO range on non-coherent memory (safe no-op otherwise).
    if (uboOffset > 0)
        vmaFlushAllocation(ctx.allocator(), uboAlloc_, 0, uboOffset);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    for (VkFramebuffer fb : framebuffers)
        vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);

    return cube;
}

void VkSceneCapture::destroy(VkContext& ctx) {
    if (uboBuffer_) {
        vmaDestroyBuffer(ctx.allocator(), uboBuffer_, uboAlloc_);
        uboBuffer_ = VK_NULL_HANDLE; uboAlloc_ = VK_NULL_HANDLE; uboMapped_ = nullptr;
    }
    if (descPool_)           { vkDestroyDescriptorPool(ctx.device(), descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    // --- Geometry pipeline ---
    if (pipeline_)           { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_)     { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)          { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    if (vert_)               { vkDestroyShaderModule(ctx.device(), vert_, nullptr); vert_ = VK_NULL_HANDLE; }
    if (frag_)               { vkDestroyShaderModule(ctx.device(), frag_, nullptr); frag_ = VK_NULL_HANDLE; }
    // --- Skybox pipeline ---
    if (skyPipeline_)        { vkDestroyPipeline(ctx.device(), skyPipeline_, nullptr); skyPipeline_ = VK_NULL_HANDLE; }
    if (skyPipelineLayout_)  { vkDestroyPipelineLayout(ctx.device(), skyPipelineLayout_, nullptr); skyPipelineLayout_ = VK_NULL_HANDLE; }
    if (skySetLayout_)       { vkDestroyDescriptorSetLayout(ctx.device(), skySetLayout_, nullptr); skySetLayout_ = VK_NULL_HANDLE; }
    if (skyIndexBuffer_)     { vmaDestroyBuffer(ctx.allocator(), skyIndexBuffer_, skyIndexAlloc_); skyIndexBuffer_ = VK_NULL_HANDLE; skyIndexAlloc_ = VK_NULL_HANDLE; }
    if (skyVertexBuffer_)    { vmaDestroyBuffer(ctx.allocator(), skyVertexBuffer_, skyVertexAlloc_); skyVertexBuffer_ = VK_NULL_HANDLE; skyVertexAlloc_ = VK_NULL_HANDLE; }
    // --- Depth + render pass ---
    if (depthView_)          { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_)         { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; depthAlloc_ = VK_NULL_HANDLE; }
    if (renderPass_)         { vkDestroyRenderPass(ctx.device(), renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    depthSize_ = 0;
}

}  // namespace iron
