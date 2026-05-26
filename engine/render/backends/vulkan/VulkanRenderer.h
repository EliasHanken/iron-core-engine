#pragma once

#include "render/Renderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkPipeline.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkDebugLines.h"
#include "render/backends/vulkan/VkHud.h"
#include "render/backends/vulkan/VkTexture.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace iron {

class Window;

// Vulkan backend for iron::Renderer. Built only when
// IRON_RENDER_BACKEND_VULKAN is defined.
//
// M9 scope: brings up the foundation (instance, device, swapchain,
// frame ring, opaque mesh+texture+shader, single render pass) and
// runs games/01-spinning-cube. Stubbed methods (cubemap, skybox,
// shadow, reflection, debug lines, HUD) log a one-time warning and
// return safely; they land in subsequent milestones.
class VulkanRenderer : public Renderer {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

    bool init(Window& window);
    bool initOk() const { return initOk_; }

    // --- resource creation ---
    MeshHandle createMesh(const MeshData& data) override;
    void updateMesh(MeshHandle mesh, const MeshData& data) override;
    TextureHandle createTexture(int width, int height,
                                const unsigned char* rgba) override;
    TextureHandle loadTexture(const std::string& path) override;
    TextureHandle whiteTexture() const override;
    TextureHandle flatNormalTexture() const override;
    TextureHandle noSpecularTexture() const override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;
    CubemapHandle createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) override;
    void setSkybox(CubemapHandle sky) override;

    // --- per-frame ---
    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Fog& fog,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;

    void setShadowBounds(Vec3 center, float radius) override;
    void setReflectionPlane(Vec3 normal, float d) override;
    void disableReflectionPlane() override;

    // --- debug ---
    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;

    // --- HUD ---
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;

    void setViewport(int width, int height) override;

    // --- engine-internal accessors (not part of iron::Renderer) ---

    // Returns the current frame's primary command buffer. Only meaningful
    // between Renderer::beginFrame and Renderer::endFrame. Used by
    // external Vulkan subsystems (e.g., iron::ParticleSystem) that need
    // to record draws into the active render pass.
    VkCommandBuffer currentCommandBuffer();

    // Exposes the frame ring so external Vulkan subsystems can allocate
    // per-frame UBO storage and descriptor sets that live until the
    // next time this frame index is reused.
    VkFrameRing& frameRing();

    // Engine-internal: expose the VkContext so external Vulkan subsystems
    // can allocate their own VMA buffers + Vulkan objects.
    VkContext& context();

    // Engine-internal: the render pass for the scene's main color+depth pass.
    // External subsystems creating their own graphics pipelines reuse it so
    // their draws go into the same framebuffer.
    VkRenderPass scenePass() const;

private:
    void warnOnce(const char* feature);
    bool recreateSwapchainAndFramebuffers(int width, int height);

    bool initOk_ = false;
    std::unordered_set<std::string> warnedFeatures_;

    VkContext    context_;
    VkSwapchain  swapchain_;
    VkFrameRing  frames_;
    VkPipeline   pipelines_;
    VkMeshStore     meshes_;
    VkTextureStore  textures_;
    VkShaderStore   shaders_;
    VkDebugLines    debugLines_;
    VkHud           hud_;

    // Per-frame transient state. beginFrame records into the active
    // command buffer; submit() and external systems' record paths
    // (e.g., iron::ParticleSystem::render) read these when computing
    // their MVPs / camera UBOs.
    Vec3      pendingClear_{0,0,0};
    Mat4      pendingView_       = Mat4::identity();
    Mat4      pendingProjection_ = Mat4::identity();

    // M12 — directional light + ambient stored at beginFrame, packed
    // into each draw's LitUbo by submit.
    Vec3 pendingSunDir_   = {0.0f, -1.0f, 0.0f};
    Vec3 pendingSunColor_ = {1.0f, 1.0f, 1.0f};
    Vec3 pendingAmbient_  = {0.1f, 0.1f, 0.1f};

    // Swapchain image index acquired in beginFrame, used in endFrame.
    std::uint32_t currentImageIndex_ = 0;
    bool       pendingResize_  = false;
    int        pendingResizeWidth_ = 0;
    int        pendingResizeHeight_ = 0;
    bool       skipFrame_ = false;  // set when acquire fails this frame
};

}  // namespace iron
