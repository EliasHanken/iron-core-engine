// VulkanRenderer.cpp — top-level orchestrator for the Vulkan backend.
// In M9 most methods are stubs; foundation tasks 4-11 fill in init
// and the per-frame pipeline.

#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"
#include "core/Window.h"
#include "math/Transform.h"

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>

namespace iron {

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        vkDeviceWaitIdle(context_.device());
        meshes_.destroyAll(context_);
        skinnedMeshes_.destroyAll(context_);  // M23
        textures_.destroyAll(context_);
        cubemaps_.destroyAll(context_);
        shaders_.destroyAll(context_);
        hud_.destroy(context_);
        skybox_.destroy(context_);
        shadowMap_.destroy(context_);
        debugLines_.destroy(context_);
        if (reflectionPipeline_)       vkDestroyPipeline(context_.device(), reflectionPipeline_, nullptr);
        if (reflectionPipelineLayout_) vkDestroyPipelineLayout(context_.device(), reflectionPipelineLayout_, nullptr);
        if (reflectionSetLayout_)      vkDestroyDescriptorSetLayout(context_.device(), reflectionSetLayout_, nullptr);
        if (reflectionVertModule_)     vkDestroyShaderModule(context_.device(), reflectionVertModule_, nullptr);
        if (reflectionFragModule_)     vkDestroyShaderModule(context_.device(), reflectionFragModule_, nullptr);
        reflection_.destroy(context_);
        pipelines_.destroy(context_);
        frames_.destroy(context_);
        swapchain_.destroy(context_);
    }
    context_.shutdown();
}

bool VulkanRenderer::init(Window& window) {
    if (!context_.init(window)) {
        Log::error("VulkanRenderer: VkContext init failed");
        return false;
    }
    if (!swapchain_.init(context_, window.width(), window.height())) {
        Log::error("VulkanRenderer: VkSwapchain init failed");
        return false;
    }
    if (!frames_.init(context_)) {
        Log::error("VulkanRenderer: VkFrameRing init failed");
        return false;
    }
    if (!pipelines_.init(context_, swapchain_)) {
        Log::error("VulkanRenderer: VkPipeline init failed");
        return false;
    }
    if (!textures_.init(context_)) {
        Log::error("VulkanRenderer: VkTextureStore init failed");
        return false;
    }
    if (!cubemaps_.init(context_)) {
        Log::error("VulkanRenderer: VkCubemapStore init failed");
        return false;
    }
    if (!shadowMap_.init(context_)) {
        Log::error("VulkanRenderer: VkShadowMap init failed");
        return false;
    }
    if (!debugLines_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkDebugLines init failed");
        return false;
    }
    if (!hud_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkHud init failed");
        return false;
    }
    if (!skybox_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkSkybox init failed");
        return false;
    }
    if (!reflection_.init(context_, textures_.sampler())) {
        Log::error("VulkanRenderer: VkReflectionTarget init failed");
        return false;
    }
    if (!buildReflectionPipeline()) {
        Log::error("VulkanRenderer: reflection pipeline build failed");
        return false;
    }

    // M17 — perform a one-shot empty clear of the reflection target so its
    // color image transitions from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    // (per the renderpass finalLayout). This avoids a Vulkan validation
    // warning on the first scene draw when no plane is set (the shader
    // still skips the sample via reflectionParams.x, but the binding is
    // written every frame).
    {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = context_.graphicsFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(context_.device(), &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(context_.device(), &cbi, &cb));

        VkCommandBufferBeginInfo bb{};
        bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bb));
        const float clearC[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        reflection_.beginPass(cb, clearC);
        reflection_.endPass(cb);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(context_.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(context_.graphicsQueue()));
        vkDestroyCommandPool(context_.device(), pool, nullptr);
    }

    initOk_ = true;
    Log::info("VulkanRenderer: context + swapchain + frames + pipeline + textures up");
    return true;
}

void VulkanRenderer::warnOnce(const char* feature) {
    if (warnedFeatures_.insert(feature).second) {
        Log::warn("Vulkan: %s not implemented yet (stub)", feature);
    }
}

// --- resource creation (real) ---

MeshHandle VulkanRenderer::createMesh(const MeshData& data) {
    return meshes_.create(context_, data);
}
void VulkanRenderer::updateMesh(MeshHandle h, const MeshData& data) {
    meshes_.update(context_, h, data);
}
TextureHandle VulkanRenderer::createTexture(int width, int height,
                                             const unsigned char* rgba) {
    return textures_.createFromRgba(context_, width, height, rgba);
}
TextureHandle VulkanRenderer::loadTexture(const std::string& path) {
    return textures_.loadFromFile(context_, path);
}
TextureHandle VulkanRenderer::whiteTexture()      const { return textures_.whiteTexture();      }
TextureHandle VulkanRenderer::flatNormalTexture() const { return textures_.flatNormalTexture(); }
TextureHandle VulkanRenderer::noSpecularTexture() const { return textures_.noSpecularTexture(); }
ShaderHandle VulkanRenderer::createShader(const std::string& v,
                                           const std::string& f) {
    return shaders_.create(context_, v, f);
}

// --- M23 skinned mesh + shader + submit ---

SkinnedMeshHandle VulkanRenderer::createSkinnedMesh(const SkinnedMeshData& data) {
    return skinnedMeshes_.create(context_, data);
}

ShaderHandle VulkanRenderer::createSkinnedShader(const std::string& vertexSrc,
                                                  const std::string& fragmentSrc) {
    return shaders_.createSkinned(context_, vertexSrc, fragmentSrc);
}

bool VulkanRenderer::reloadShader(ShaderHandle handle,
                                  const std::string& vertexSrc,
                                  const std::string& fragmentSrc) {
    if (!shaders_.has(handle)) {
        Log::error("VulkanRenderer::reloadShader: unknown handle %u", handle);
        return false;
    }
    // Wait for the GPU to finish using the current modules + pipeline before
    // we tear them down. Dev-only flow; the stall is acceptable.
    vkDeviceWaitIdle(context_.device());

    if (!shaders_.reload(context_, handle, vertexSrc, fragmentSrc)) {
        return false;  // compile error — last-good shader preserved
    }
    // The VkShader address is stable across reload; drop its cached pipeline
    // so the next draw rebuilds with the new modules.
    pipelines_.invalidate(context_, &shaders_.get(handle));
    return true;
}

void VulkanRenderer::submitSkinnedDraw(const SkinnedDrawCall& call) {
    if (skipFrame_) return;
    if (!skinnedMeshes_.has(call.skinnedMesh) || !shaders_.has(call.shader)) return;
    skinnedDraws_.push_back(call);
    skinnedBoneMatricesStash_.emplace_back(call.boneMatrices.begin(),
                                            call.boneMatrices.end());
}

// --- M9 stubs (M10+ work) ---

CubemapHandle VulkanRenderer::createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) {
    return cubemaps_.createFromFaces(context_, width, height, faces);
}

void VulkanRenderer::setSkybox(CubemapHandle sky) {
    pendingSkybox_ = sky;
}
void VulkanRenderer::setShadowBounds(Vec3 center, float radius) {
    pendingShadowCenter_ = center;
    pendingShadowRadius_ = radius;
}
void VulkanRenderer::setReflectionPlane(Vec3 normal, float d) {
    ReflectionPlane plane;
    plane.normal = normal;
    plane.d = d;
    reflectionPlane_ = plane;
}

void VulkanRenderer::disableReflectionPlane() {
    reflectionPlane_.reset();
}
void VulkanRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.queue(a, b, color);
}

void VulkanRenderer::drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.queueOverlay(a, b, color);
}

void VulkanRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    if (skipFrame_) return;
    pendingDebugView_  = view;
    pendingDebugProj_  = projection;
    pendingDebugFlush_ = true;
}
void VulkanRenderer::drawHud(const HudBatch& batch, int fbW, int fbH) {
    if (skipFrame_) return;
    pendingHudBatch_ = batch;
    pendingHudW_     = fbW;
    pendingHudH_     = fbH;
    pendingHudValid_ = true;
}

// --- per-frame (real) ---

namespace {

// M12+M13+M14+M15+M17 — per-draw UBO uploaded by submit. std140 layout:
// all members are mat4 (64-byte aligned) or vec4 (16-byte aligned), so no
// straddling. Total 928 bytes (M17 added reflectionViewProj + reflectionParams + clipPlane).
struct LitUbo {
    Mat4 mvp;                 // 64
    Mat4 model;               // 64
    Mat4 lightViewProj;       // 64
    Vec4 sunDir;              // 16
    Vec4 sunColor;            // 16
    Vec4 ambient;             // 16
    Vec4 emissive;            // 16
    Vec4 cameraPos;           // 16
    Vec4 materialParams;      // 16  x=uvScale, y=specPower, z=reflectivity, w=shadowBias
    Vec4 fogColor;            // 16  M15 — xyz=color, w=density
    Vec4 lightCounts;         // 16  M15 — x=pointLightCount (as float), y/z/w padding
    Vec4 pointPositions[16];  // 256 M15 — xyz=position, w=intensity
    Vec4 pointColors[16];     // 256 M15 — xyz=color, w=range
    Mat4 reflectionViewProj;  // 64  M17 — scene: identity; reflection: P * V * mirror
    Vec4 reflectionParams;    // 16  M17 — x=useReflectionPlane (0/1), y=screenW, z=screenH, w=0
    Vec4 clipPlane;           // 16  M17 — (normal.xyz, -d) for reflection pass; ignored in scene
};
static_assert(sizeof(LitUbo) == 928, "LitUbo std140 layout (M17)");

// Extracts the camera's world-space position from a view matrix.
// Assumes view is a pure rigid transform [R | t; 0 0 0 1] (rotation +
// translation, no scale) — true for all engine cameras (lookAt /
// first-person / free-fly). For column-major Mat4 storage where
// m[col*4+row] = at(row, col), the camera world position is -R^T * t.
Vec3 extractCameraPos(const Mat4& view) {
    const float tx = view.at(0, 3);
    const float ty = view.at(1, 3);
    const float tz = view.at(2, 3);
    return Vec3{
        -(view.at(0, 0) * tx + view.at(1, 0) * ty + view.at(2, 0) * tz),
        -(view.at(0, 1) * tx + view.at(1, 1) * ty + view.at(2, 1) * tz),
        -(view.at(0, 2) * tx + view.at(1, 2) * ty + view.at(2, 2) * tz),
    };
}

// Builds the directional light's orthographic view-projection matrix.
// `dir` is the sun direction (engine convention: direction the light
// travels). Eye = center - dir * 2r so the camera sits behind the scene
// relative to the sun. Matches the OpenGL backend's computeLightViewProj.
Mat4 computeLightViewProj(Vec3 dir, Vec3 center, float radius) {
    const Vec3 dn = normalize(dir);
    const Vec3 up = (std::fabs(dn.y) > 0.99f)
        ? Vec3{0.0f, 0.0f, 1.0f}
        : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 eye{
        center.x - dn.x * (radius * 2.0f),
        center.y - dn.y * (radius * 2.0f),
        center.z - dn.z * (radius * 2.0f),
    };
    const Mat4 view = lookAt(eye, center, up);
    const Mat4 proj = orthographic(-radius, radius,
                                   -radius, radius,
                                   radius * 0.5f, radius * 3.5f);
    return proj * view;
}

// Set viewport + scissor on the active command buffer. Vulkan clip-Y points
// DOWN (opposite OpenGL); negative-height + bottom-origin viewport flips it
// back so GL-style projection matrices render correctly without altering
// winding (back-face culling stays consistent).
void setSceneViewport(VkCommandBuffer cb, VkExtent2D extent) {
    VkViewport vp{};
    vp.x = 0;
    vp.y = static_cast<float>(extent.height);
    vp.width  = static_cast<float>(extent.width);
    vp.height = -static_cast<float>(extent.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

const char* kReflectionVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

// Full LitUbo std140 layout (928 bytes after M17). We reference only
// `model`, `reflectionViewProj`, and `clipPlane`.
layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
} u;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormal;

void main() {
    vec4 worldPos = u.model * vec4(aPos, 1.0);
    vUV = aUV;
    vNormal = mat3(u.model) * aNormal;
    gl_ClipDistance[0] = dot(worldPos.xyz, u.clipPlane.xyz) + u.clipPlane.w;
    gl_Position = u.reflectionViewProj * worldPos;
}
)";

const char* kReflectionFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormal;
layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, normalize(vec3(0.3, 1.0, 0.2))), 0.0);
    vec4 texel = texture(uDiffuse, vUV);
    outColor = vec4(texel.rgb * (0.3 + 0.7 * diffuse), texel.a);
}
)";
}  // namespace

void VulkanRenderer::beginFrame(Vec3 clearColor, const DirectionalLight& light,
                                 std::span<const PointLight> pointLights,
                                 const Fog& fog, const Mat4& view,
                                 const Mat4& projection) {
    pendingClear_      = clearColor;
    pendingView_       = view;
    pendingProjection_ = projection;
    pendingSunDir_     = light.direction;
    pendingSunColor_   = light.color;
    pendingAmbient_    = Vec3{light.ambient * light.color.x,
                              light.ambient * light.color.y,
                              light.ambient * light.color.z};
    pendingCameraPos_ = extractCameraPos(view);
    pendingLightViewProj_ = computeLightViewProj(
        pendingSunDir_, pendingShadowCenter_, pendingShadowRadius_);
    pendingPointLightCount_ = static_cast<int>(
        std::min<std::size_t>(pointLights.size(), kMaxPointLights));
    for (int i = 0; i < pendingPointLightCount_; ++i) {
        pendingPointLights_[i] = pointLights[i];
    }
    pendingFog_ = fog;
    skipFrame_         = false;

    if (pendingResize_) {
        vkDeviceWaitIdle(context_.device());
        swapchain_.recreate(context_, pendingResizeWidth_, pendingResizeHeight_);
        pipelines_.recreateFramebuffers(context_, swapchain_);
        pendingResize_ = false;
    }

    // Wait for the current frame's previous use to complete; reset.
    VkFrameRing::Frame& f = frames_.current();
    vkWaitForFences(context_.device(), 1, &f.inFlight, VK_TRUE, UINT64_MAX);
    vkResetFences(context_.device(), 1, &f.inFlight);
    frames_.resetCurrentFrame(context_);

    const VkResult r = vkAcquireNextImageKHR(
        context_.device(), swapchain_.handle(), UINT64_MAX,
        f.imageAvailable, VK_NULL_HANDLE, &currentImageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        pendingResize_ = true;
        pendingResizeWidth_  = static_cast<int>(swapchain_.extent().width);
        pendingResizeHeight_ = static_cast<int>(swapchain_.extent().height);
        skipFrame_ = true;
        return;
    } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        Log::error("Vulkan: vkAcquireNextImageKHR failed (%s)",
                   vkResultString(r));
        skipFrame_ = true;
        return;
    }

    // Begin recording. Render pass and draw replay happen in endFrame.
    VkCommandBuffer cb = f.commandBuffer;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    sceneDraws_.clear();
    skinnedDraws_.clear();
    skinnedBoneMatricesStash_.clear();
    deferredScenePass_.clear();
    pendingDebugFlush_ = false;
    pendingHudValid_   = false;
}

void VulkanRenderer::submit(const DrawCall& call) {
    if (skipFrame_) return;
    sceneDraws_.push_back(call);
}

void VulkanRenderer::recordSceneDraw(VkCommandBuffer cb, const DrawCall& call) {
    if (!meshes_.has(call.mesh) || !shaders_.has(call.shader)) return;

    VkFrameRing::Frame& f = frames_.current();

    LitUbo ubo;
    ubo.mvp      = pendingProjection_ * pendingView_ * call.model;
    ubo.model    = call.model;
    ubo.sunDir   = Vec4{pendingSunDir_.x,   pendingSunDir_.y,   pendingSunDir_.z,   0.0f};
    ubo.sunColor = Vec4{pendingSunColor_.x, pendingSunColor_.y, pendingSunColor_.z, 0.0f};
    ubo.ambient  = Vec4{pendingAmbient_.x,  pendingAmbient_.y,  pendingAmbient_.z,  0.0f};
    ubo.emissive = Vec4{call.material.emissive.x,
                        call.material.emissive.y,
                        call.material.emissive.z,
                        0.0f};
    ubo.lightViewProj = pendingLightViewProj_;
    ubo.cameraPos = Vec4{pendingCameraPos_.x,
                         pendingCameraPos_.y,
                         pendingCameraPos_.z,
                         0.0f};
    ubo.materialParams = Vec4{
        call.material.uvScale,
        call.material.specPower,
        call.material.reflectivity,
        pendingShadowBias_,
    };
    ubo.fogColor = Vec4{
        pendingFog_.color.x, pendingFog_.color.y, pendingFog_.color.z,
        pendingFog_.density,
    };
    ubo.lightCounts = Vec4{
        static_cast<float>(pendingPointLightCount_), 0.0f, 0.0f, 0.0f,
    };
    for (int i = 0; i < pendingPointLightCount_; ++i) {
        const PointLight& pl = pendingPointLights_[i];
        ubo.pointPositions[i] = Vec4{
            pl.position.x, pl.position.y, pl.position.z, pl.intensity,
        };
        ubo.pointColors[i] = Vec4{
            pl.color.x, pl.color.y, pl.color.z, pl.range,
        };
    }
    // Zero unused slots so the GPU never reads uninitialized stack data.
    for (int i = pendingPointLightCount_; i < kMaxPointLights; ++i) {
        ubo.pointPositions[i] = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
        ubo.pointColors[i]    = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
    }

    // M17 — scene pass: pass-through reflectionViewProj (unused by scene
    // shader), planar-reflection flag from material, screen size for
    // projective sampling. clipPlane is ignored in the scene pass.
    ubo.reflectionViewProj = Mat4::identity();
    const float useRefl = (call.material.useReflectionPlane && reflectionPlane_.has_value())
                          ? 1.0f : 0.0f;
    ubo.reflectionParams = Vec4{
        useRefl,
        static_cast<float>(swapchain_.extent().width),
        static_cast<float>(swapchain_.extent().height),
        0.0f,
    };
    ubo.clipPlane = Vec4{0.0f, 0.0f, 0.0f, 0.0f};

    const VkShader& sh = shaders_.get(call.shader);
    ::VkPipeline pipe = pipelines_.pipelineFor(context_, swapchain_, sh);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    // Allocate + write descriptor set from the frame's pool.
    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = f.descriptorPool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts = &sh.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(context_.device(), &dsInfo, &set));

    const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = f.uboBuffer;
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(ubo);

    // M13 — write 4 descriptors per draw: UBO + diffuse + normal + spec.
    // Fallback textures used for invalid handles.
    const auto& diffuse = textures_.has(call.material.texture)
        ? textures_.get(call.material.texture)
        : textures_.get(textures_.whiteTexture());
    const auto& normal = textures_.has(call.material.normalMap)
        ? textures_.get(call.material.normalMap)
        : textures_.get(textures_.flatNormalTexture());
    const auto& spec = textures_.has(call.material.specularMap)
        ? textures_.get(call.material.specularMap)
        : textures_.get(textures_.noSpecularTexture());

    // M16 — also bind the active skybox (or black fallback) at binding 5.
    const CubemapHandle skyHandle = cubemaps_.has(pendingSkybox_)
        ? pendingSkybox_
        : cubemaps_.blackCubemap();
    const auto& skyTex = cubemaps_.get(skyHandle);

    VkDescriptorImageInfo imgInfos[6]{};
    imgInfos[0].sampler     = diffuse.sampler;
    imgInfos[0].imageView   = diffuse.view;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = normal.sampler;
    imgInfos[1].imageView   = normal.view;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = spec.sampler;
    imgInfos[2].imageView   = spec.view;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[3].sampler     = shadowMap_.sampler();
    imgInfos[3].imageView   = shadowMap_.depthView();
    imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[4].sampler     = skyTex.sampler;
    imgInfos[4].imageView   = skyTex.view;
    imgInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // M17 — binding 6: planar reflection RTT (or a stale/empty texture
    // when no plane is active — shader guards via reflectionParams.x).
    imgInfos[5] = reflection_.descriptorImageInfo();

    VkWriteDescriptorSet writes[7]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    for (int i = 0; i < 6; ++i) {
        writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i + 1].dstSet = set;
        writes[i + 1].dstBinding = i + 1;
        writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].pImageInfo = &imgInfos[i];
    }
    vkUpdateDescriptorSets(context_.device(), 7, writes, 0, nullptr);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            sh.pipelineLayout, 0, 1, &set, 0, nullptr);

    const auto& mesh = meshes_.get(call.mesh);
    VkDeviceSize offsets[1] = {0};
    vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
    vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
}

// M23 — record one skinned draw. Mirrors recordSceneDraw: same LitUbo,
// same 7 scene-pass descriptors, plus an 8th UBO binding (binding 7)
// of bone matrices (vertex stage). Uses skinnedPipelineFor (SkinnedVertex
// input layout) and binds the skinned vertex+index buffers.
void VulkanRenderer::recordSkinnedDraw(VkCommandBuffer cb,
                                         const SkinnedDrawCall& call,
                                         const std::vector<Mat4>& bones) {
    if (!skinnedMeshes_.has(call.skinnedMesh) || !shaders_.has(call.shader)) return;

    VkFrameRing::Frame& f = frames_.current();

    // --- 1. Build LitUbo (copied verbatim from recordSceneDraw) ---
    LitUbo ubo;
    ubo.mvp      = pendingProjection_ * pendingView_ * call.model;
    ubo.model    = call.model;
    ubo.sunDir   = Vec4{pendingSunDir_.x,   pendingSunDir_.y,   pendingSunDir_.z,   0.0f};
    ubo.sunColor = Vec4{pendingSunColor_.x, pendingSunColor_.y, pendingSunColor_.z, 0.0f};
    ubo.ambient  = Vec4{pendingAmbient_.x,  pendingAmbient_.y,  pendingAmbient_.z,  0.0f};
    ubo.emissive = Vec4{call.material.emissive.x,
                        call.material.emissive.y,
                        call.material.emissive.z,
                        0.0f};
    ubo.lightViewProj = pendingLightViewProj_;
    ubo.cameraPos = Vec4{pendingCameraPos_.x,
                         pendingCameraPos_.y,
                         pendingCameraPos_.z,
                         0.0f};
    ubo.materialParams = Vec4{
        call.material.uvScale,
        call.material.specPower,
        call.material.reflectivity,
        pendingShadowBias_,
    };
    ubo.fogColor = Vec4{
        pendingFog_.color.x, pendingFog_.color.y, pendingFog_.color.z,
        pendingFog_.density,
    };
    ubo.lightCounts = Vec4{
        static_cast<float>(pendingPointLightCount_), 0.0f, 0.0f, 0.0f,
    };
    for (int i = 0; i < pendingPointLightCount_; ++i) {
        const PointLight& pl = pendingPointLights_[i];
        ubo.pointPositions[i] = Vec4{
            pl.position.x, pl.position.y, pl.position.z, pl.intensity,
        };
        ubo.pointColors[i] = Vec4{
            pl.color.x, pl.color.y, pl.color.z, pl.range,
        };
    }
    for (int i = pendingPointLightCount_; i < kMaxPointLights; ++i) {
        ubo.pointPositions[i] = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
        ubo.pointColors[i]    = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
    }
    ubo.reflectionViewProj = Mat4::identity();
    const float useRefl = (call.material.useReflectionPlane && reflectionPlane_.has_value())
                          ? 1.0f : 0.0f;
    ubo.reflectionParams = Vec4{
        useRefl,
        static_cast<float>(swapchain_.extent().width),
        static_cast<float>(swapchain_.extent().height),
        0.0f,
    };
    ubo.clipPlane = Vec4{0.0f, 0.0f, 0.0f, 0.0f};

    const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));

    // --- 2. Build bone-matrix UBO. Pad to kMaxBonesPerSkinnedMesh with
    // identity so the shader can index safely even if weights point past
    // the actual joint count. ---
    std::array<Mat4, kMaxBonesPerSkinnedMesh> bonePadded;
    for (auto& m : bonePadded) m = Mat4::identity();
    const std::size_t copyN = std::min(bones.size(), kMaxBonesPerSkinnedMesh);
    for (std::size_t i = 0; i < copyN; ++i) bonePadded[i] = bones[i];
    const VkDeviceSize bonesOffset = frames_.allocateUbo(
        bonePadded.data(), sizeof(Mat4) * kMaxBonesPerSkinnedMesh);

    // --- 3. Bind pipeline + allocate descriptor set ---
    const VkShader& sh = shaders_.get(call.shader);
    ::VkPipeline pipe = pipelines_.skinnedPipelineFor(context_, swapchain_, sh);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = f.descriptorPool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts = &sh.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(context_.device(), &dsInfo, &set));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = f.uboBuffer;
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(ubo);

    // --- 4. Resolve textures (same fallbacks as scene path) ---
    const auto& diffuse = textures_.has(call.material.texture)
        ? textures_.get(call.material.texture)
        : textures_.get(textures_.whiteTexture());
    const auto& normal = textures_.has(call.material.normalMap)
        ? textures_.get(call.material.normalMap)
        : textures_.get(textures_.flatNormalTexture());
    const auto& spec = textures_.has(call.material.specularMap)
        ? textures_.get(call.material.specularMap)
        : textures_.get(textures_.noSpecularTexture());

    const CubemapHandle skyHandle = cubemaps_.has(pendingSkybox_)
        ? pendingSkybox_
        : cubemaps_.blackCubemap();
    const auto& skyTex = cubemaps_.get(skyHandle);

    VkDescriptorImageInfo imgInfos[6]{};
    imgInfos[0].sampler     = diffuse.sampler;
    imgInfos[0].imageView   = diffuse.view;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = normal.sampler;
    imgInfos[1].imageView   = normal.view;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = spec.sampler;
    imgInfos[2].imageView   = spec.view;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[3].sampler     = shadowMap_.sampler();
    imgInfos[3].imageView   = shadowMap_.depthView();
    imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[4].sampler     = skyTex.sampler;
    imgInfos[4].imageView   = skyTex.view;
    imgInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[5] = reflection_.descriptorImageInfo();

    VkDescriptorBufferInfo bonesInfo{};
    bonesInfo.buffer = f.uboBuffer;
    bonesInfo.offset = bonesOffset;
    bonesInfo.range  = sizeof(Mat4) * kMaxBonesPerSkinnedMesh;

    VkWriteDescriptorSet writes[8]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    for (int i = 0; i < 6; ++i) {
        writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i + 1].dstSet = set;
        writes[i + 1].dstBinding = i + 1;
        writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].pImageInfo = &imgInfos[i];
    }
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = set;
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[7].descriptorCount = 1;
    writes[7].pBufferInfo = &bonesInfo;
    vkUpdateDescriptorSets(context_.device(), 8, writes, 0, nullptr);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            sh.pipelineLayout, 0, 1, &set, 0, nullptr);

    // --- 5. Bind vertex/index buffers + draw ---
    const auto& mesh = skinnedMeshes_.get(call.skinnedMesh);
    VkDeviceSize offsets[1] = {0};
    vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
    vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
}

void VulkanRenderer::endFrame() {
    if (skipFrame_) {
        frames_.advance();
        return;
    }

    VkFrameRing::Frame& f = frames_.current();
    VkCommandBuffer cb = f.commandBuffer;

    // --- Pass 1: shadow ---
    shadowMap_.record(cb, context_.device(), frames_, meshes_,
                     pendingLightViewProj_, sceneDraws_);

    // --- Pass 2: planar reflection (if a plane is set) ---
    if (reflectionPlane_.has_value()) {
        const ReflectionPlane& plane = *reflectionPlane_;
        const Mat4 mirror = reflectionMatrix(plane);
        const Mat4 reflectionVP = pendingProjection_ * (pendingView_ * mirror);

        const float clearC[4] = {pendingClear_.x, pendingClear_.y,
                                 pendingClear_.z, 1.0f};
        reflection_.beginPass(cb, clearC);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          reflectionPipeline_);

        for (const DrawCall& call : sceneDraws_) {
            if (call.material.useReflectionPlane) continue;  // skip mirror itself
            if (!meshes_.has(call.mesh)) continue;

            // Build a mini-LitUbo: only model, reflectionViewProj, clipPlane
            // are read by the reflection vert shader. Zero the rest.
            LitUbo ubo{};
            ubo.model = call.model;
            ubo.reflectionViewProj = reflectionVP;
            ubo.clipPlane = Vec4{plane.normal.x, plane.normal.y, plane.normal.z,
                                  -plane.d};

            const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = f.uboBuffer;
            bufInfo.offset = uboOffset;
            bufInfo.range  = sizeof(ubo);

            VkDescriptorSetAllocateInfo dsInfo{};
            dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsInfo.descriptorPool = f.descriptorPool;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts = &reflectionSetLayout_;
            VkDescriptorSet rset = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(context_.device(), &dsInfo, &rset));

            const auto& diffuse = textures_.has(call.material.texture)
                ? textures_.get(call.material.texture)
                : textures_.get(textures_.whiteTexture());

            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler     = diffuse.sampler;
            imgInfo.imageView   = diffuse.view;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet rWrites[2]{};
            rWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rWrites[0].dstSet = rset;
            rWrites[0].dstBinding = 0;
            rWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            rWrites[0].descriptorCount = 1;
            rWrites[0].pBufferInfo = &bufInfo;
            rWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rWrites[1].dstSet = rset;
            rWrites[1].dstBinding = 1;
            rWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            rWrites[1].descriptorCount = 1;
            rWrites[1].pImageInfo = &imgInfo;
            vkUpdateDescriptorSets(context_.device(), 2, rWrites, 0, nullptr);

            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    reflectionPipelineLayout_, 0, 1, &rset, 0, nullptr);

            const auto& mesh = meshes_.get(call.mesh);
            VkDeviceSize offsets[1] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
            vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
        }

        reflection_.endPass(cb);
    }

    // --- Scene pass ---
    {
        VkClearValue clears[2]{};
        clears[0].color.float32[0] = pendingClear_.x;
        clears[0].color.float32[1] = pendingClear_.y;
        clears[0].color.float32[2] = pendingClear_.z;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = pipelines_.renderPass();
        rpBegin.framebuffer = pipelines_.framebuffer(currentImageIndex_);
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = swapchain_.extent();
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        setSceneViewport(cb, swapchain_.extent());

        // M16 — draw skybox first inside the scene pass.
        if (cubemaps_.has(pendingSkybox_)) {
            Mat4 v = pendingView_;
            v.at(0, 3) = 0.0f;
            v.at(1, 3) = 0.0f;
            v.at(2, 3) = 0.0f;
            const Mat4 vp = pendingProjection_ * v;
            skybox_.record(cb, context_.device(), frames_,
                          cubemaps_.get(pendingSkybox_), vp);
        }

        for (const DrawCall& call : sceneDraws_) {
            recordSceneDraw(cb, call);
        }

        // M23 — skinned draws replay after static ones, before deferred passes.
        for (std::size_t i = 0; i < skinnedDraws_.size(); ++i) {
            recordSkinnedDraw(cb, skinnedDraws_[i], skinnedBoneMatricesStash_[i]);
        }

        for (auto& fn : deferredScenePass_) {
            fn(cb);
        }
        deferredScenePass_.clear();

        // A deferred pass may change the viewport/scissor — notably ImGui's
        // Vulkan backend sets a full-screen POSITIVE-height viewport + per-draw
        // scissor. Restore the scene's negative-height (clip-Y-flipped) viewport
        // and full scissor so debug-lines and the HUD render with the same
        // convention as the scene geometry (otherwise they appear Y-mirrored).
        setSceneViewport(cb, swapchain_.extent());

        if (pendingDebugFlush_) {
            debugLines_.record(cb, context_.device(), frames_,
                               pendingDebugView_, pendingDebugProj_);
            pendingDebugFlush_ = false;
        }
        if (pendingHudValid_) {
            hud_.record(cb, context_.device(), frames_, textures_,
                        pendingHudBatch_, pendingHudW_, pendingHudH_);
            pendingHudValid_ = false;
        }

        vkCmdEndRenderPass(cb);
    }

    VK_CHECK(vkEndCommandBuffer(cb));

    const VkPipelineStageFlags waitStages[] =
        {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.imageAvailable;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &f.renderFinished;
    VK_CHECK(vkQueueSubmit(context_.graphicsQueue(), 1, &submit, f.inFlight));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &f.renderFinished;
    present.swapchainCount = 1;
    VkSwapchainKHR sc = swapchain_.handle();
    present.pSwapchains = &sc;
    present.pImageIndices = &currentImageIndex_;
    const VkResult r = vkQueuePresentKHR(context_.presentQueue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        pendingResize_ = true;
        pendingResizeWidth_  = static_cast<int>(swapchain_.extent().width);
        pendingResizeHeight_ = static_cast<int>(swapchain_.extent().height);
    } else if (r != VK_SUCCESS) {
        Log::error("Vulkan: vkQueuePresentKHR failed (%s)", vkResultString(r));
    }

    frames_.advance();
}

void VulkanRenderer::setViewport(int width, int height) {
    pendingResize_ = true;
    pendingResizeWidth_ = width;
    pendingResizeHeight_ = height;
}

VkCommandBuffer VulkanRenderer::currentCommandBuffer() {
    // Returns VK_NULL_HANDLE during a skipped frame (acquire failed,
    // resize pending). External subsystems must check the return and
    // skip their own recording to avoid issuing commands on a buffer
    // that was never vkBeginCommandBuffer-ed.
    if (skipFrame_) return VK_NULL_HANDLE;
    return frames_.current().commandBuffer;
}

VkFrameRing& VulkanRenderer::frameRing() {
    return frames_;
}

VkContext& VulkanRenderer::context() { return context_; }

VkRenderPass VulkanRenderer::scenePass() const {
    return pipelines_.renderPass();
}

void VulkanRenderer::enqueueDeferredScenePass(
        std::function<void(VkCommandBuffer)> fn) {
    if (skipFrame_) return;
    deferredScenePass_.push_back(std::move(fn));
}

bool VulkanRenderer::buildReflectionPipeline() {
    // --- Descriptor set layout: UBO (0) + diffuse (1) ---
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 2;
    sli.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(context_.device(), &sli, nullptr,
                                          &reflectionSetLayout_));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &reflectionSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(context_.device(), &pli, nullptr,
                                     &reflectionPipelineLayout_));

    // --- Compile shaders to SPIR-V via the engine's existing helper ---
    auto vertSpv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kReflectionVert);
    if (vertSpv.empty()) {
        Log::error("VulkanRenderer: reflection vert SPIR-V compile failed");
        return false;
    }
    auto fragSpv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kReflectionFrag);
    if (fragSpv.empty()) {
        Log::error("VulkanRenderer: reflection frag SPIR-V compile failed");
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertSpv.size() * sizeof(uint32_t);
    smInfo.pCode    = vertSpv.data();
    VK_CHECK(vkCreateShaderModule(context_.device(), &smInfo, nullptr,
                                   &reflectionVertModule_));
    smInfo.codeSize = fragSpv.size() * sizeof(uint32_t);
    smInfo.pCode    = fragSpv.data();
    VK_CHECK(vkCreateShaderModule(context_.device(), &smInfo, nullptr,
                                   &reflectionFragModule_));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = reflectionVertModule_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = reflectionFragModule_;
    stages[1].pName = "main";

    // --- Vertex input — match VkMesh layout (Pos, Normal, UV, Tangent) ---
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 11 * sizeof(float);  // 3+3+2+3 = 11 floats
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    6 * sizeof(float)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 * sizeof(float)};

    VkPipelineVertexInputStateCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vinfo.vertexBindingDescriptionCount = 1;
    vinfo.pVertexBindingDescriptions = &bind;
    vinfo.vertexAttributeDescriptionCount = 4;
    vinfo.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo iaInfo{};
    iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpInfo{};
    vpInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpInfo.viewportCount = 1;
    vpInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // mirror flips winding; disable cull
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

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState   = &vinfo;
    gp.pInputAssemblyState = &iaInfo;
    gp.pViewportState      = &vpInfo;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dynInfo;
    gp.layout = reflectionPipelineLayout_;
    gp.renderPass = reflection_.renderPass();
    gp.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE,
                                        1, &gp, nullptr, &reflectionPipeline_));
    return true;
}

}  // namespace iron
