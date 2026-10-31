// VkVolumeRenderer.cpp — volumetric raymarch and upsample (see VolumeRenderer.h).

#include "render/backends/vulkan/VkVolumeRenderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFluidVolume.h"
#include "render/backends/vulkan/VkLightVolume.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include "core/Log.h"

namespace iron {

namespace {

constexpr std::uint32_t kLocalSize = 8;
constexpr std::uint32_t kShadowWidth = 1024;
constexpr std::uint32_t kShadowHeight = 512;

const char* const kRaymarchGlsl = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depthTexture;
layout(set = 0, binding = 1) uniform sampler3D lightVolume;
layout(set = 0, binding = 2) uniform sampler3D scalarsVolume;
layout(set = 0, binding = 3) uniform sampler3D solidVolume;
layout(rgba16f, set = 0, binding = 4) writeonly uniform image2D result;
layout(set = 0, binding = 6) uniform sampler2D shadowMap;
layout(set = 0, binding = 5) uniform Ubo {
    mat4 invViewProjection;
    vec4 cameraPos;
    vec4 lightMin;
    vec4 lightInvSize;
    vec4 fluidMin;      // w > 0 when there is a fluid
    vec4 fluidInvSize;
    vec4 smokeAlbedo;   // w = smoke density
    vec4 heatColor;     // w = heat intensity
    vec4 shadowLight;   // w = shadow density
    vec4 pointLight;    // rgb = direct light on smoke
    vec4 params;        // x = air density, y = light intensity, z = detail, w = detail scale
    vec4 shape;         // x = phase, y = powder, z = air light, w = shadows on
    ivec4 counts;       // x = steps, y = frame index
} u;

float gradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float hash31(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i + vec3(0, 0, 0)), hash31(i + vec3(1, 0, 0)), f.x),
            mix(hash31(i + vec3(0, 1, 0)), hash31(i + vec3(1, 1, 0)), f.x), f.y),
        mix(mix(hash31(i + vec3(0, 0, 1)), hash31(i + vec3(1, 0, 1)), f.x),
            mix(hash31(i + vec3(0, 1, 1)), hash31(i + vec3(1, 1, 1)), f.x), f.y),
        f.z);
}

bool boxInterval(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out float t0, out float t1) {
    vec3 inv = 1.0 / rd;
    vec3 a = (lo - ro) * inv;
    vec3 b = (hi - ro) * inv;
    vec3 near = min(a, b);
    vec3 far = max(a, b);
    t0 = max(max(near.x, near.y), max(near.z, 0.0));
    t1 = min(far.x, min(far.y, far.z));
    return t0 < t1;
}

// 1 where the light reaches this point, read from a map of the distance to the
// nearest occluder in every direction from the light.
float lightVisibility(vec3 p) {
    if (u.shape.w <= 0.0) return 1.0;
    vec3 d = p - u.shadowLight.xyz;
    float r = length(d);
    d /= max(r, 1e-3);
    vec2 uv = vec2(atan(d.z, d.x) / (2.0 * 3.14159265) + 0.5,
                   acos(clamp(d.y, -1.0, 1.0)) / 3.14159265);
    float occluder = textureLod(shadowMap, uv, 0.0).r;
    return 1.0 - smoothstep(occluder, occluder + 1.4, r);
}

// Henyey-Greenstein, scaled to 1 for even scattering. Gas glows brightest when
// looking toward the light, which is how real haze and smoke behave.
float scatterPhase(float cosine) {
    float g = u.shape.x;
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5);
}

// Thin, eroded density: edges break into wisps finer than the grid.
float smokeAt(vec3 p, vec3 uvw) {
    float density = texture(scalarsVolume, uvw).r;
    if (density < 0.002 || u.params.z <= 0.0) return density;
    float noise = valueNoise(p * u.params.w) * 0.5 + valueNoise(p * u.params.w * 2.3) * 0.3 +
                  valueNoise(p * u.params.w * 5.1) * 0.2;
    return max(density - u.params.z * noise * min(density * 2.0 + 0.1, 1.0), 0.0);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(result);
    if (any(greaterThanEqual(pixel, size))) return;

    ivec2 fullPixel = pixel * 2;
    vec2 fullSize = vec2(textureSize(depthTexture, 0));
    float depth = texelFetch(depthTexture, fullPixel, 0).r;

    // The scene's viewport is flipped, so clip Y runs opposite to image rows.
    vec2 uv = (vec2(fullPixel) + 0.5) / fullSize;
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 world = u.invViewProjection * clip;
    world /= world.w;

    vec3 ro = u.cameraPos.xyz;
    vec3 toSurface = world.xyz - ro;
    float surface = length(toSurface);
    vec3 rd = toSurface / surface;
    if (depth >= 1.0) surface = 1e7;

    vec3 boundsMax = u.lightMin.xyz + 1.0 / u.lightInvSize.xyz;
    float t0, t1;
    if (!boxInterval(ro, rd, u.lightMin.xyz, boundsMax, t0, t1)) {
        imageStore(result, pixel, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    t1 = min(t1, surface);
    if (t1 <= t0) {
        imageStore(result, pixel, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    bool hasFluid = u.fluidMin.w > 0.0;
    int steps = u.counts.x;
    float stepLength = (t1 - t0) / float(steps);
    float jitter = gradientNoise(vec2(pixel) + 5.588238 * float(u.counts.y % 64));

    float air = u.params.x;
    vec3 light = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < steps; ++i) {
        vec3 p = ro + rd * (t0 + (float(i) + jitter) * stepLength);
        vec3 radiance = texture(lightVolume, (p - u.lightMin.xyz) * u.lightInvSize.xyz).rgb * u.params.y;

        float smoke = 0.0;
        float heat = 0.0;
        if (hasFluid) {
            vec3 uvw = (p - u.fluidMin.xyz) * u.fluidInvSize.xyz;
            if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
                if (texture(solidVolume, uvw).r > 0.5) break;
                // Fading out near the walls means the box itself can never show.
                vec3 toWall = min(uvw, 1.0 - uvw);
                float edge = smoothstep(0.0, 0.08, min(toWall.x, min(toWall.y, toWall.z)));
                smoke = smokeAt(p, uvw) * edge;
                heat = texture(scalarsVolume, uvw).g * edge;
            }
        }

        float extinction = air + smoke * u.smokeAlbedo.w;
        if (extinction <= 1e-5 && heat <= 1e-4) continue;

        float stepTransmittance = exp(-extinction * stepLength);

        vec3 toLight = u.shadowLight.xyz - p;
        float toLightLength = length(toLight);
        toLight /= max(toLightLength, 1e-3);
        vec3 direct = u.pointLight.rgb / (1.0 + toLightLength * toLightLength * 0.1);
        direct *= scatterPhase(dot(rd, toLight)) * lightVisibility(p);

        vec3 scattered = radiance * air + direct * air * u.shape.z;

        if (smoke > 1e-3) {
            // Self-shadowing: this is what gives smoke dark folds and bright edges.
            float shadowStep = min(toLightLength, 4.0) / 6.0;
            float occlusion = 0.0;
            for (int s = 1; s <= 6; ++s) {
                vec3 q = p + toLight * shadowStep * (float(s) - 0.5 + jitter);
                occlusion += texture(scalarsVolume, (q - u.fluidMin.xyz) * u.fluidInvSize.xyz).r;
            }
            float optical = occlusion * shadowStep * u.shadowLight.w;
            float shadow = exp(-optical);
            // Beer-powder: thin gas lets light straight through, so its edges read
            // darker and the billows gain the rounded look of real smoke.
            float powder = mix(1.0, 1.0 - exp(-optical * 2.0 - smoke * 2.0), u.shape.y);
            scattered += (radiance + direct) * smoke * u.smokeAlbedo.w * u.smokeAlbedo.rgb * shadow * powder;
        }

        // Gas hot enough to glow by itself. It whitens as it heats.
        vec3 ember = mix(u.heatColor.rgb, vec3(1.0, 0.70, 0.35), clamp(heat * 0.12, 0.0, 1.0));
        vec3 emission = ember * heat * heat * u.heatColor.w;

        float integral = extinction > 1e-5 ? (1.0 - stepTransmittance) / extinction : stepLength;
        light += transmittance * (scattered + emission) * integral;
        transmittance *= stepTransmittance;
        if (transmittance < 0.01) break;
    }

    imageStore(result, pixel, vec4(light, transmittance));
}
)GLSL";

// For every direction from the light, the distance to the nearest sphere.
const char* const kOccluderGlsl = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(r16f, set = 0, binding = 0) writeonly uniform image2D result;
layout(set = 0, binding = 1) uniform Ubo {
    vec4 light;
    ivec4 count;
    vec4 spheres[256];
} u;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(result);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    float azimuth = (uv.x - 0.5) * 2.0 * 3.14159265;
    float polar = uv.y * 3.14159265;
    vec3 dir = vec3(sin(polar) * cos(azimuth), cos(polar), sin(polar) * sin(azimuth));

    float nearest = 1000.0;
    for (int i = 0; i < u.count.x; ++i) {
        vec3 toCenter = u.spheres[i].xyz - u.light.xyz;
        float along = dot(toCenter, dir);
        if (along <= 0.0) continue;
        float offAxis = dot(toCenter, toCenter) - along * along;
        float radius2 = u.spheres[i].w * u.spheres[i].w;
        if (offAxis < radius2) nearest = min(nearest, along - sqrt(radius2 - offAxis));
    }
    imageStore(result, pixel, vec4(nearest));
}
)GLSL";

const char* const kUpsampleVertGlsl = R"GLSL(#version 450
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Takes the four nearest half-resolution pixels, trusting those at this pixel's
// depth. Without the depth weight, smoke would bleed across rock silhouettes.
const char* const kUpsampleFragGlsl = R"GLSL(#version 450
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D volumeTexture;
layout(set = 0, binding = 1) uniform sampler2D depthTexture;
layout(push_constant) uniform Push { vec2 projection; } push;

float linearDepth(float d) { return push.projection.y / (d + push.projection.x); }

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 halfSize = textureSize(volumeTexture, 0);
    float center = linearDepth(texelFetch(depthTexture, pixel, 0).r);

    vec2 position = gl_FragCoord.xy * 0.5 - 0.5;
    ivec2 base = ivec2(floor(position));
    vec2 f = position - vec2(base);

    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            ivec2 cell = clamp(base + ivec2(x, y), ivec2(0), halfSize - 1);
            float z = linearDepth(texelFetch(depthTexture, cell * 2, 0).r);
            float difference = abs(z - center) / max(center, 0.1);
            float weight = mix(1.0 - f.x, f.x, float(x)) * mix(1.0 - f.y, f.y, float(y)) *
                           (1.0 / (1e-3 + difference * difference * 400.0));
            sum += texelFetch(volumeTexture, cell, 0) * weight;
            total += weight;
        }
    }
    vec4 value = sum / max(total, 1e-5);
    outColor = vec4(value.rgb, 1.0 - value.a);
}
)GLSL";

struct RaymarchUbo {
    Mat4  invViewProjection;
    float cameraPos[4];
    float lightMin[4];
    float lightInvSize[4];
    float fluidMin[4];
    float fluidInvSize[4];
    float smokeAlbedo[4];
    float heatColor[4];
    float shadowLight[4];
    float pointLight[4];
    float params[4];
    float shape[4];
    std::int32_t counts[4];
};

struct OccluderUbo {
    float        light[4];
    std::int32_t count[4];
    float        spheres[VolumeRenderer::kMaxOccluders][4];
};

void setVec4(float out[4], Vec3 v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

Vec3 inverseExtent(const VolumeBounds& b) {
    return {1.0f / (b.max.x - b.min.x), 1.0f / (b.max.y - b.min.y), 1.0f / (b.max.z - b.min.z)};
}

}  // namespace

VolumeRendererSources volumeRendererSources() {
    return {kRaymarchGlsl, kOccluderGlsl, kUpsampleVertGlsl, kUpsampleFragGlsl};
}

VkVolumeRenderer::~VkVolumeRenderer() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();
    vkDeviceWaitIdle(ctx.device());

    if (upsamplePipeline_) vkDestroyPipeline(ctx.device(), upsamplePipeline_, nullptr);
    if (upsampleLayout_) vkDestroyPipelineLayout(ctx.device(), upsampleLayout_, nullptr);
    if (upsampleSetLayout_) vkDestroyDescriptorSetLayout(ctx.device(), upsampleSetLayout_, nullptr);
    raymarchStage_.destroy(ctx);
    occluderStage_.destroy(ctx);
    shadowMap_.destroy(ctx);
    target_.destroy(ctx);
}

bool VkVolumeRenderer::init(VulkanRenderer& renderer) {
    renderer_ = &renderer;

    constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (!raymarchStage_.init(renderer.context(), kRaymarchGlsl,
                             {kSampled, kSampled, kSampled, kSampled,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              kSampled})) {
        return false;
    }
    if (!occluderStage_.init(renderer.context(), kOccluderGlsl,
                             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER})) {
        return false;
    }
    if (!shadowMap_.create2D(renderer.context(), kShadowWidth, kShadowHeight, VK_FORMAT_R16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    return createUpsamplePipeline();
}

bool VkVolumeRenderer::createUpsamplePipeline() {
    VkContext& ctx = renderer_->context();

    VkDescriptorSetLayoutBinding b[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        b[i].binding         = i;
        b[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &setInfo, nullptr, &upsampleSetLayout_));

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size       = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &upsampleSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr, &upsampleLayout_));

    const auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kUpsampleVertGlsl);
    const auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kUpsampleFragGlsl);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VolumeRenderer: upsample shader compile failed");
        return false;
    }

    VkShaderModule vmod = VK_NULL_HANDLE, fmod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    moduleInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &moduleInfo, nullptr, &vmod));
    moduleInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    moduleInfo.pCode    = fspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &moduleInfo, nullptr, &fmod));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName  = "main";

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
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

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
    info.layout              = upsampleLayout_;
    info.renderPass          = renderer_->transparentPass();
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                       &upsamplePipeline_));

    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);
    return upsamplePipeline_ != VK_NULL_HANDLE;
}

bool VkVolumeRenderer::ensureTarget() {
    const VkExtent2D full = renderer_->sceneExtent();
    const VkExtent2D half{std::max(full.width / 2, 1u), std::max(full.height / 2, 1u)};
    if (half.width == targetExtent_.width && half.height == targetExtent_.height) return true;

    VkContext& ctx = renderer_->context();
    vkDeviceWaitIdle(ctx.device());
    target_.destroy(ctx);
    if (!target_.create2D(ctx, half.width, half.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    targetExtent_ = half;
    return true;
}

void VkVolumeRenderer::setOccluders(Vec3 light, std::span<const Vec4> spheres, float strength) {
    VkCommandBuffer cb = renderer_->currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;

    if (spheres.empty()) {
        shadowsOn_ = false;
        renderer_->setPointShadow(VK_NULL_HANDLE, light, 0.0f);
        return;
    }

    auto ubo = std::make_unique<OccluderUbo>();
    setVec4(ubo->light, light, 0.0f);
    const std::size_t count = std::min<std::size_t>(spheres.size(), kMaxOccluders);
    ubo->count[0] = static_cast<std::int32_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
        ubo->spheres[i][0] = spheres[i].x;
        ubo->spheres[i][1] = spheres[i].y;
        ubo->spheres[i][2] = spheres[i].z;
        ubo->spheres[i][3] = spheres[i].w;
    }

    VkFrameRing& ring = renderer_->frameRing();
    const VkDeviceSize offset = ring.allocateUbo(ubo.get(), sizeof(OccluderUbo));

    shadowMap_.discard();
    shadowMap_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT);
    occluderStage_.dispatch(*renderer_, cb,
                            {VkComputeResource::image(shadowMap_.view()),
                             VkComputeResource::bufferRange(ring.current().uboBuffer, offset,
                                                            sizeof(OccluderUbo))},
                            nullptr, (kShadowWidth + kLocalSize - 1) / kLocalSize,
                            (kShadowHeight + kLocalSize - 1) / kLocalSize);
    shadowMap_.transition(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT);

    shadowsOn_ = true;
    renderer_->setPointShadow(shadowMap_.view(), light, strength);
}

void VkVolumeRenderer::render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                              const FluidVolume* fluid, const VolumeRenderSettings& settings) {
    if (renderer_->currentCommandBuffer() == VK_NULL_HANDLE) return;
    if (!ensureTarget()) return;

    const auto& vkLight = static_cast<const VkLightVolume&>(light);
    const auto* vkFluid = static_cast<const VkFluidVolume*>(fluid);
    if (vkFluid && !vkFluid->hasStepped()) vkFluid = nullptr;

    const Mat4 viewProjection = projection * view;
    const Mat4 invView = iron::inverse(view);
    const Vec3 camera{invView.at(0, 3), invView.at(1, 3), invView.at(2, 3)};

    RaymarchUbo ubo{};
    ubo.invViewProjection = iron::inverse(viewProjection);
    setVec4(ubo.cameraPos, camera, 1.0f);
    setVec4(ubo.lightMin, light.bounds().min, 0.0f);
    setVec4(ubo.lightInvSize, inverseExtent(light.bounds()), 0.0f);
    if (vkFluid) {
        setVec4(ubo.fluidMin, vkFluid->bounds().min, 1.0f);
        setVec4(ubo.fluidInvSize, inverseExtent(vkFluid->bounds()), 0.0f);
    }
    setVec4(ubo.smokeAlbedo, settings.smokeAlbedo, settings.smokeDensity);
    setVec4(ubo.heatColor, settings.heatColor, settings.heatIntensity);
    setVec4(ubo.shadowLight, settings.lightPosition, settings.shadowDensity);
    setVec4(ubo.pointLight, settings.lightColor, 0.0f);
    ubo.params[0] = settings.airDensity;
    ubo.params[1] = settings.lightIntensity;
    ubo.params[2] = settings.detail;
    ubo.params[3] = settings.detailScale;
    ubo.shape[0] = settings.phase;
    ubo.shape[1] = settings.powder;
    ubo.shape[2] = settings.airLight;
    ubo.shape[3] = shadowsOn_ ? 1.0f : 0.0f;
    ubo.counts[0] = settings.steps;
    ubo.counts[1] = static_cast<std::int32_t>(frameIndex_++);

    VkFrameRing& ring = renderer_->frameRing();
    const VkDeviceSize uboOffset = ring.allocateUbo(&ubo, sizeof(ubo));
    const VkBuffer uboBuffer = ring.current().uboBuffer;

    const VkImageView lightView = vkLight.resolvedView();
    const VkSampler volumeSampler = renderer_->volumeSampler();
    const VkSampler depthSampler = renderer_->depthSampler();
    const VkImageView depthView = renderer_->sceneDepthView();
    const VkImageView scalarsView = vkFluid ? vkFluid->scalarsView() : renderer_->blackVolumeView();
    const VkImageView solidView = vkFluid ? vkFluid->solidView() : renderer_->blackVolumeView();
    const VkImageLayout fluidLayout = vkFluid ? VK_IMAGE_LAYOUT_GENERAL
                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Before the first setOccluders the map has never been written. The raymarch
    // ignores it then, but the descriptor must still describe the image correctly.
    const VkImageLayout shadowLayout = shadowsOn_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_GENERAL;
    if (!shadowsOn_) {
        shadowMap_.discard();
        shadowMap_.transition(renderer_->currentCommandBuffer(), VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    renderer_->enqueueSceneCompute([=, this](VkCommandBuffer cb) {
        // Rewritten in full each frame, so its old contents can go.
        target_.discard();
        target_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT);

        using R = VkComputeResource;
        raymarchStage_.dispatch(
            *renderer_, cb,
            {R::sampled(depthView, depthSampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
             R::sampled(lightView, volumeSampler),
             R::sampled(scalarsView, volumeSampler, fluidLayout),
             R::sampled(solidView, volumeSampler, fluidLayout), R::image(target_.view()),
             R::bufferRange(uboBuffer, uboOffset, sizeof(RaymarchUbo)),
             R::sampled(shadowMap_.view(), volumeSampler, shadowLayout)},
            nullptr, (targetExtent_.width + kLocalSize - 1) / kLocalSize,
            (targetExtent_.height + kLocalSize - 1) / kLocalSize);

        target_.transition(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    });

    const float a = projection.at(2, 2);
    const float b = projection.at(2, 3);

    renderer_->enqueueDeferredScenePass([this, depthView, depthSampler, a, b](VkCommandBuffer cb) {
        VkContext& ctx = renderer_->context();
        VkFrameRing::Frame& frame = renderer_->frameRing().current();

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = frame.descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &upsampleSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &allocInfo, &set));

        VkDescriptorImageInfo images[2]{};
        images[0] = {renderer_->volumeSampler(), target_.view(),
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {depthSampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[2]{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &images[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

        const float push[2] = {a, b};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, upsamplePipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, upsampleLayout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cb, upsampleLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        vkCmdDraw(cb, 3, 1, 0, 0);
    });
}

std::unique_ptr<VolumeRenderer> createVolumeRenderer(Renderer& renderer) {
    auto* vk = dynamic_cast<VulkanRenderer*>(&renderer);
    if (!vk) {
        Log::error("createVolumeRenderer: renderer is not a VulkanRenderer");
        return nullptr;
    }
    auto volumes = std::make_unique<VkVolumeRenderer>();
    if (!volumes->init(*vk)) return nullptr;
    return volumes;
}

}  // namespace iron
