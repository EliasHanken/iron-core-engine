// VulkanRenderer.cpp — top-level orchestrator for the Vulkan backend.
// In M9 most methods are stubs; foundation tasks 4-11 fill in init
// and the per-frame pipeline.

#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"
#include "core/Window.h"
#include "math/Transform.h"

#include <vulkan/vulkan.h>
#include <cmath>

namespace iron {

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        vkDeviceWaitIdle(context_.device());
        meshes_.destroyAll(context_);
        textures_.destroyAll(context_);
        shaders_.destroyAll(context_);
        hud_.destroy(context_);
        shadowMap_.destroy(context_);
        debugLines_.destroy(context_);
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

// --- M9 stubs (M10+ work) ---

CubemapHandle VulkanRenderer::createCubemap(int, int,
    const std::array<const unsigned char*, 6>&) {
    warnOnce("createCubemap");
    return kInvalidHandle;
}
void VulkanRenderer::setSkybox(CubemapHandle) { warnOnce("setSkybox"); }
void VulkanRenderer::setShadowBounds(Vec3 center, float radius) {
    pendingShadowCenter_ = center;
    pendingShadowRadius_ = radius;
}
void VulkanRenderer::setReflectionPlane(Vec3, float) { warnOnce("setReflectionPlane"); }
void VulkanRenderer::disableReflectionPlane() { warnOnce("disableReflectionPlane"); }
void VulkanRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.queue(a, b, color);
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

// M12+M13+M14 — per-draw UBO uploaded by submit. std140 layout: all members
// are mat4 (64-byte aligned) or vec4 (16-byte aligned), so no
// straddling. Total 288 bytes.
struct LitUbo {
    Mat4 mvp;             // 64
    Mat4 model;           // 64
    Mat4 lightViewProj;   // 64  M14
    Vec4 sunDir;          // 16
    Vec4 sunColor;        // 16
    Vec4 ambient;         // 16
    Vec4 emissive;        // 16
    Vec4 cameraPos;       // 16
    Vec4 materialParams;  // 16  x=uvScale, y=specPower, z=reflectivity, w=shadowBias (M14)
};
static_assert(sizeof(LitUbo) == 288, "LitUbo std140 layout");

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
}  // namespace

void VulkanRenderer::beginFrame(Vec3 clearColor, const DirectionalLight& light,
                                 std::span<const PointLight>,
                                 const Fog&, const Mat4& view,
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

    VkDescriptorImageInfo imgInfos[4]{};
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

    VkWriteDescriptorSet writes[5]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imgInfos[0];
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imgInfos[1];
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &imgInfos[2];
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &imgInfos[3];
    vkUpdateDescriptorSets(context_.device(), 5, writes, 0, nullptr);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            sh.pipelineLayout, 0, 1, &set, 0, nullptr);

    const auto& mesh = meshes_.get(call.mesh);
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

        for (const DrawCall& call : sceneDraws_) {
            recordSceneDraw(cb, call);
        }

        for (auto& fn : deferredScenePass_) {
            fn(cb);
        }
        deferredScenePass_.clear();

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

}  // namespace iron
