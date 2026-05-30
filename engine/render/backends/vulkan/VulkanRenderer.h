#pragma once

#include "render/Renderer.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkSkinnedMesh.h"
#include "render/backends/vulkan/VkPipeline.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkDebugLines.h"
#include "render/backends/vulkan/VkHud.h"
#include "render/backends/vulkan/VkShadowMap.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkSkybox.h"
#include "render/backends/vulkan/VkReflectionTarget.h"
#include "render/backends/vulkan/VkPostProcess.h"
#include "render/ReflectionPlane.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
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
    // M23 — skinned mesh + draw API (Vulkan-only).
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) override;
    ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) override;
    bool reloadShader(ShaderHandle handle,
                      const std::string& vertexSrc,
                      const std::string& fragmentSrc) override;
    void submitSkinnedDraw(const SkinnedDrawCall& call) override;
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
    void drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) override;
    void drawLineOverlayThick(Vec3 a, Vec3 b, Vec3 color) override;
    void drawTriOverlay(Vec3 a, Vec3 b, Vec3 c, Vec3 color) override;
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

    // Engine-internal: the offscreen scene render pass (color+depth). Used by
    // subsystems whose draws are recorded in the offscreen scene pass (skybox,
    // scene geometry, particles).
    VkRenderPass scenePass() const;

    // Engine-internal: the swapchain (final) render pass — where composite,
    // debug lines, HUD, and UI/overlays record.
    VkRenderPass swapchainPass() const;

    // Engine-internal: external Vulkan subsystems register a deferred
    // render callback. Fires inside the scene render pass during endFrame,
    // after the geometry replay and before debug-lines + HUD.
    //
    // LIFETIME: the callback may capture this/other-state by reference or
    // raw pointer. The caller MUST guarantee that any captured objects
    // outlive the matching endFrame() call. The deferred queue is cleared
    // at every beginFrame() — captures only need to survive until endFrame
    // of the SAME frame.
    void enqueueDeferredScenePass(std::function<void(VkCommandBuffer)> fn);

    // Engine-internal: UI/overlay callbacks (e.g. ImGui). Fires inside the
    // swapchain pass AFTER the post-process composite, so overlays are never
    // affected by post-process effects. Cleared each beginFrame.
    void enqueueDeferredUiPass(std::function<void(VkCommandBuffer)> fn);

private:
    void warnOnce(const char* feature);
    bool recreateSwapchainAndFramebuffers(int width, int height);
    void recordSceneDraw(VkCommandBuffer cb, const DrawCall& call);
    void recordSkinnedDraw(VkCommandBuffer cb, const SkinnedDrawCall& call,
                           const std::vector<Mat4>& bones);
    bool buildReflectionPipeline();

    bool initOk_ = false;
    std::unordered_set<std::string> warnedFeatures_;

    VkContext    context_;
    VkSwapchain  swapchain_;
    VkFrameRing  frames_;
    VkPipeline   pipelines_;
    VkMeshStore     meshes_;
    VkSkinnedMeshStore skinnedMeshes_;  // M23
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

    // M13 — camera world position, extracted from view matrix at beginFrame.
    // Used by submit() for Blinn-Phong specular highlights in the lit shader.
    Vec3 pendingCameraPos_ = {0.0f, 0.0f, 0.0f};

    // M14 — directional-light shadow state.
    Vec3  pendingShadowCenter_  = {0.0f, 0.0f, 0.0f};
    float pendingShadowRadius_  = 20.0f;
    float pendingShadowBias_    = 0.002f;
    Mat4  pendingLightViewProj_ = Mat4::identity();
    VkShadowMap shadowMap_;

    // M16 — cubemap storage + skybox subsystem + currently-set skybox.
    VkCubemapStore cubemaps_;
    VkSkybox       skybox_;
    CubemapHandle  pendingSkybox_ = kInvalidHandle;

    // M36 — offscreen scene-color target + post-process composite pipeline.
    VkPostProcess         postProcess_;

    // M17 — planar reflection RTT + shared pipeline + currently-set plane.
    VkReflectionTarget    reflection_;
    VkDescriptorSetLayout reflectionSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      reflectionPipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          reflectionPipeline_ = VK_NULL_HANDLE;
    VkShaderModule        reflectionVertModule_ = VK_NULL_HANDLE;
    VkShaderModule        reflectionFragModule_ = VK_NULL_HANDLE;
    std::optional<ReflectionPlane> reflectionPlane_;

    // M15 — point lights + fog (existing beginFrame args, now stored).
    std::array<PointLight, kMaxPointLights> pendingPointLights_{};
    int  pendingPointLightCount_ = 0;
    Fog  pendingFog_{};

    // M14 — frame-flow state for defer-and-replay rendering.
    std::vector<DrawCall> sceneDraws_;
    std::vector<std::function<void(VkCommandBuffer)>> deferredScenePass_;

    // M36 -- UI/overlay callbacks recorded in the swapchain pass after composite.
    std::vector<std::function<void(VkCommandBuffer)>> deferredUiPass_;

    // M23 — buffered skinned draws + a deep-copy of each call's bone
    // matrices. SkinnedDrawCall holds a std::span, which is non-owning;
    // submitSkinnedDraw copies the matrices into the stash so the caller
    // can free the source range before endFrame replays them.
    std::vector<SkinnedDrawCall>     skinnedDraws_;
    std::vector<std::vector<Mat4>>   skinnedBoneMatricesStash_;

    Mat4 pendingDebugView_       = Mat4::identity();
    Mat4 pendingDebugProj_       = Mat4::identity();
    bool pendingDebugFlush_      = false;

    HudBatch pendingHudBatch_{};
    int      pendingHudW_      = 0;
    int      pendingHudH_      = 0;
    bool     pendingHudValid_  = false;

    // Swapchain image index acquired in beginFrame, used in endFrame.
    std::uint32_t currentImageIndex_ = 0;
    bool       pendingResize_  = false;
    int        pendingResizeWidth_ = 0;
    int        pendingResizeHeight_ = 0;
    bool       skipFrame_ = false;  // set when acquire fails this frame
};

}  // namespace iron
