#pragma once

#include "render/Cinematic.h"
#include "render/Renderer.h"
#include "render/ShadowCascades.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkSkinnedMesh.h"
#include "render/backends/vulkan/VkPipeline.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkDebugLines.h"
#include "render/backends/vulkan/VkVolumeImage.h"
#include "render/backends/vulkan/VkHud.h"
#include "render/backends/vulkan/VkShadowMap.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkIblBaker.h"
#include "render/backends/vulkan/VkSceneCapture.h"
#include "render/backends/vulkan/VkSkybox.h"
#include "render/backends/vulkan/VkReflectionTarget.h"
#include "render/backends/vulkan/VkPostProcess.h"
#include "render/backends/vulkan/VkGpuProfiler.h"
#include "render/backends/vulkan/VkClusters.h"
#include "render/backends/vulkan/VkInstanceGroups.h"
#include "render/ReflectionPlane.h"
#include "render/ReflectionProbe.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace iron {

class LightVolume;
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
                                const unsigned char* rgba,
                                bool srgb = true) override;
    TextureHandle loadTexture(const std::string& path,
                              bool srgb = true) override;
    TextureHandle whiteTexture() const override;
    TextureHandle flatNormalTexture() const override;
    TextureHandle noSpecularTexture() const override;
    // Engine-internal: resolve a texture handle's VkImageView + VkSampler (both
    // already in SHADER_READ_ONLY_OPTIMAL). Used by ImGuiLayer to bind a
    // small RGBA8 texture as an ImGui texture id. Returns false if unknown.
    bool textureViewSampler(TextureHandle h, VkImageView& view,
                            VkSampler& sampler) const;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;
    // M23 — skinned mesh + draw API (Vulkan-only).
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) override;
    ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) override;
    // M50b — tessellated shader (vert + tesc + tese + frag).
    ShaderHandle createTessellatedShader(const std::string& vert,
                                          const std::string& tesc,
                                          const std::string& tese,
                                          const std::string& frag) override;
    bool reloadShader(ShaderHandle handle,
                      const std::string& vertexSrc,
                      const std::string& fragmentSrc) override;
    void submitSkinnedDraw(const SkinnedDrawCall& call) override;
    CubemapHandle createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) override;
    void setSkybox(CubemapHandle sky) override;
    CubemapHandle loadHdrSkybox(const std::string& hdrPath, int faceSize) override;

    // M49 — reflection probe API.
    void setReflectionProbes(std::span<const GpuReflectionProbe> probes) override;
    void bakeReflectionProbes(std::vector<GpuReflectionProbe>& probes) override;

    // --- per-frame ---
    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Fog& fog,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;

    void setShadowBounds(Vec3 center, float radius) override;
    void setShadowCascades(float distance) override { shadowCascadeDistance_ = distance; }
    void setReflectionPlane(Vec3 normal, float d) override;
    void disableReflectionPlane() override;

    // M50b — wireframe + tessellation factor.
    void setWireframe(bool e) override { pendingWireframe_ = e; }
    void setTessellationFactor(float f) override { pendingTessFactor_ = f; }
    // M50c — adaptive mode + target NDC edge length.
    void setTessellationMode(bool adaptive) override { pendingTessAdaptive_ = adaptive; }
    void setTessellationTargetEdge(float ndc) override { pendingTessTargetEdge_ = ndc; }

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

    std::span<const GpuTiming> gpuTimings() const override { return profiler_.latest(); }

    ShaderHandle createInstancedShader(const std::string& vertexSrc, const std::string& fragmentSrc) override;
    InstanceGroupHandle createInstanceGroup(const InstanceGroupDesc& desc) override;
    void setInstances(InstanceGroupHandle group, std::span<const Mat4> transforms) override;
    CullStats cullStats() const override { return instanceGroups_.stats(); }
    void setCullingFrozen(bool frozen) override { instanceGroups_.setFrozen(frozen); }
    void setCullingCamera(const CullingCamera* camera) override {
        cullingCamera_ = camera ? std::optional<CullingCamera>(*camera) : std::nullopt;
    }
    void setAntiAliasing(AntiAliasing mode) override;
    void setSharpness(float amount) override { postProcess_.setTaaSharpness(amount); }
    void setVolumetricFog(const VolumetricFog& fog) override { volumetricFog_ = fog; }
    void submitFogVolume(const FogVolume& volume) override {
        if (!skipFrame_) pendingFogVolumes_.push_back(volume);
    }
    void submitFogDisturbance(const FogDisturbance& disturbance) override {
        if (!skipFrame_) pendingFogDisturbances_.push_back(disturbance);
    }
    void submitFogFluid(const FogFluid& fluid) override {
        if (!skipFrame_ && fluid.fluid) pendingFogFluids_.push_back(fluid);
    }
    void submitDecal(const Decal& decal) override {
        if (!skipFrame_) pendingDecals_.push_back(decal);
    }
    bool setDecalTextures(std::span<const std::string> paths) override {
        return clusters_.setDecalTextures(context_, paths);
    }
    void setViewModelFov(float verticalRadians) override { viewModelFov_ = verticalRadians; }
    void resetTemporalHistory() override;

    // M36 — stores an effect style; looked up each frame via runChain.
    void setEffectStyle(uint8_t effectId, const EffectStyle& style) override;

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

    // Engine-internal: the swapchain (final) render pass — where the viewport
    // blit + UI/overlays (ImGui) record. M43a: composite, debug lines, and HUD
    // moved to the offscreen viewport pass; their pipelines stay built against
    // THIS pass and record into the viewport pass via render-pass compatibility
    // (same color+depth formats — finalLayout/ops don't affect compatibility).
    VkRenderPass swapchainPass() const;

    // M43a: the final composited scene+overlays image, sampleable. M43b's
    // ImGui Viewport panel binds this via ImGui_ImplVulkan_AddTexture.
    VkImageView viewportColorView() const;
    VkSampler   viewportSampler() const;

    // M43a: resize the offscreen viewport target independently of the swapchain.
    // M43b drives this from the Viewport panel's content size. No-op on
    // unchanged/zero extent. Calls vkDeviceWaitIdle internally.
    void resizeViewport(uint32_t width, uint32_t height);

    // How the composited viewport image reaches the swapchain:
    //   true  (default) — blit it full-screen in the swapchain pass. Games
    //                     present their scene this way (no editor UI).
    //   false           — the host composites it itself (the editor draws it
    //                     via ImGui::Image), so endFrame skips the blit and
    //                     the swapchain pass carries only ImGui.
    void setBlitViewportToSwapchain(bool blit) { blitViewportToSwapchain_ = blit; }

    // Linear exposure multiply applied before the ACES tonemap in the composite
    // step (M44). Default 1.0. Auto-exposure is deferred.
    void setExposure(float exposure) { pendingExposure_ = exposure; }

    // M47 bloom knobs (VulkanRenderer-only, mirror setExposure). Threshold/knee
    // gate the bright-pass; intensity scales the bloom added before ACES;
    // scatter biases the up-sample blend toward wider/tighter spread.
    void setBloomThreshold(float v) { pendingBloomThreshold_ = v; }
    void setBloomKnee(float v)      { pendingBloomKnee_ = v; }
    void setBloomIntensity(float v) { pendingBloomIntensity_ = v; }
    void setBloomScatter(float v)   { pendingBloomScatter_ = v; }

    // M48 SSAO knobs (VulkanRenderer-only, mirror the bloom knobs). radius/bias/
    // power tune the occlusion estimate; strength scales the AO applied in the
    // composite (mix(1, ao, strength)).
    void setSsaoRadius(float v)   { pendingSsaoRadius_ = v; }
    void setSsaoBias(float v)     { pendingSsaoBias_ = v; }
    void setSsaoPower(float v)    { pendingSsaoPower_ = v; }
    void setSsaoStrength(float v) { pendingSsaoStrength_ = v; }

    // Bakes `skyGlsl`, which defines `vec3 skyColor(vec3 dir);`, into an HDR
    // cubemap usable with setSkybox. Blocks until the GPU finishes.
    CubemapHandle bakeSky(const std::string& skyGlsl, int faceSize);

    // Applies a film camera to the composite. See CinematicSettings.
    void setCinematic(const CinematicSettings& settings) { cinematic_ = settings; }

    // Shows the renderer's own data in place of the picture (needs the cinematic
    // composite enabled). See RenderDebugView.
    void setDebugView(RenderDebugView view) {
        debugView_ = view;
        postProcess_.setDebugView(view);
    }

    // Engine-internal: subsystems that record their own GPU work can time it
    // with this. Scopes must be balanced within a frame.
    VkGpuProfiler& gpuProfiler() { return profiler_; }

    // Engine-internal: shadows on the first point light, from a latitude-longitude
    // map of occluder distances. Pass a null view or zero strength to clear.
    void setPointShadow(VkImageView map, Vec3 center, float strength);

    // Lights lit surfaces from `volume`, or clears it with nullptr. The volume
    // must outlive its use, and must be resolved before endFrame.
    void setLightVolume(const LightVolume* volume, float intensity);

    // Engine-internal: clamped trilinear sampler for 3D volumes.
    VkSampler volumeSampler() const { return volumeSampler_; }

    // Engine-internal: nearest sampler for reading scene depth.
    VkSampler depthSampler() const { return depthSampler_; }

    // Engine-internal: a 1x1x1 black volume, in SHADER_READ_ONLY_OPTIMAL.
    VkImageView blackVolumeView() const { return blackVolume_.view(); }

    // Copies this frame's final composited image (tonemapped, before any UI)
    // and hands it to `fn` as tightly packed RGBA8 with alpha forced to 255,
    // sRGB encoded when the swapchain is. Fires inside endFrame after a blocking
    // GPU wait, so it's for screenshots and offline recording. Call between
    // beginFrame and endFrame; `pixels` is only valid during the callback.
    using FrameCaptureFn =
        std::function<void(const std::uint8_t* pixels, int width, int height)>;
    // With `includeUi`, it copies the final window instead, UI included.
    void captureFrame(FrameCaptureFn fn, bool includeUi = false) {
        pendingCapture_ = std::move(fn);
        pendingCaptureUi_ = includeUi;
    }

    // Engine-internal: the pass transparent draws record into. Pipelines used
    // from enqueueDeferredScenePass must be built against this one.
    VkRenderPass transparentPass() const;

    // Engine-internal: scene depth, sampleable in
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL from enqueueSceneCompute and
    // enqueueDeferredScenePass callbacks.
    VkImageView sceneDepthView() const;

    // Engine-internal: the HDR scene's extent.
    VkExtent2D sceneExtent() const;

    // Engine-internal: compute recorded during endFrame between the opaque and
    // transparent passes, when scene depth is complete and readable. Cleared
    // each beginFrame. Same lifetime rule as enqueueDeferredScenePass.
    void enqueueSceneCompute(std::function<void(VkCommandBuffer)> fn);

    // Engine-internal: external Vulkan subsystems register a deferred
    // render callback. Fires inside the transparent scene pass during endFrame,
    // after the opaque geometry and before debug-lines + HUD.
    //
    // LIFETIME: the callback may capture this/other-state by reference or
    // raw pointer. The caller MUST guarantee that any captured objects
    // outlive the matching endFrame() call. The deferred queue is cleared
    // at every beginFrame() — captures only need to survive until endFrame
    // of the SAME frame.
    void enqueueDeferredScenePass(std::function<void(VkCommandBuffer)> fn);

    // Engine-internal: UI/overlay callbacks (e.g. ImGui). Fires inside the
    // swapchain pass after the viewport blit, so overlays are never composited
    // into the post-process chain. Cleared each beginFrame.
    //
    // LIFETIME: same as enqueueDeferredScenePass — captures must outlive
    // endFrame() of the same frame.
    void enqueueDeferredUiPass(std::function<void(VkCommandBuffer)> fn);

private:
    void warnOnce(const char* feature);
    bool recreateSwapchainAndFramebuffers(int width, int height);
    // (Re)creates imageRenderFinished_ to match swapchain_.imageCount().
    // Destroys any existing semaphores first; call after every swapchain
    // (re)creation. Returns false on semaphore creation failure.
    bool recreateImageSemaphores();
    // Where an instance group's draw finds its instances and its indirect command.
    struct IndirectSource {
        VkBuffer     instances;
        VkBuffer     visible;
        VkBuffer     commands;
        VkDeviceSize offset;
    };
    void recordSceneDraw(VkCommandBuffer cb, const DrawCall& call, const IndirectSource* indirect = nullptr);
    void recordSkinnedDraw(VkCommandBuffer cb, const SkinnedDrawCall& call,
                           const std::vector<Mat4>& bones);
    // M36 Phase C: records one static mesh draw into the mask pass. Pushes
    // MaskPushConstants {mvp, effectId} and issues vkCmdDrawIndexed.
    void recordMaskDraw(VkCommandBuffer cb, const DrawCall& call);
    bool buildReflectionPipeline();
    bool initVolumeDefaults();
    void bindLightVolume(VkDescriptorSet set);
    // Frame capture: recordCaptureCopy runs after the viewport pass, then
    // deliverCapture runs after the submit once the GPU has finished.
    bool recordCaptureCopy(VkCommandBuffer cb, VkImage image, VkExtent2D extent,
                           VkImageLayout layout, VkPipelineStageFlags nextStage);
    void deliverCapture();
    void destroyCaptureBuffer();

    bool initOk_ = false;

    const LightVolume* lightVolume_ = nullptr;
    float              lightVolumeIntensity_ = 0.0f;
    VkVolumeImage      blackVolume_;
    VkSampler          volumeSampler_ = VK_NULL_HANDLE;
    VkSampler          depthSampler_ = VK_NULL_HANDLE;
    VkImageView        pointShadowView_ = VK_NULL_HANDLE;
    Vec3               pointShadowCenter_{};
    float              pointShadowStrength_ = 0.0f;
    CinematicSettings  cinematic_;
    VkGpuProfiler      profiler_;
    VkInstanceGroups   instanceGroups_;
    VkClusters         clusters_;
    std::vector<PointLight> pendingAllLights_;  // Every light this frame, for the clusters.
    std::vector<Decal>      pendingDecals_;
    VolumetricFog           volumetricFog_;
    std::vector<FogVolume>  pendingFogVolumes_;
    std::vector<FogDisturbance> pendingFogDisturbances_;
    std::vector<FogFluid> pendingFogFluids_;
    std::optional<CullingCamera> cullingCamera_;  // Culls instead of the drawing camera, when set.
    float                   fogClock_ = 0.0f;
    RenderDebugView    debugView_ = RenderDebugView::None;
    MotionHistory      motionHistory_;
    PaletteHistory     paletteHistory_;
    std::vector<VelocityObject> velocityObjects_;
    // Last frame's view-projection, without its jitter.
    Mat4               previousViewProjection_ = Mat4::identity();
    bool               hasPreviousViewProjection_ = false;
    AntiAliasing       antiAliasing_ = AntiAliasing::Off;
    std::uint32_t      jitterFrame_ = 0;
    Mat4               pendingJitter_ = Mat4::identity();          // This frame's, or identity.
    Mat4               pendingUnjitteredProjection_ = Mat4::identity();
    // The view model's projection this frame (jittered like the world's), and last
    // frame's view-projection for it, unjittered.
    float              viewModelFov_ = 0.0f;
    Mat4               pendingViewModelProjection_ = Mat4::identity();
    Mat4               previousViewModelViewProjection_ = Mat4::identity();
    Mat4               viewModelUnjittered_ = Mat4::identity();
    const Mat4& projectionFor(bool viewModel) const {
        return viewModel ? pendingViewModelProjection_ : pendingProjection_;
    }
    std::uint32_t      cinematicFrame_ = 0;

    FrameCaptureFn pendingCapture_;
    bool           pendingCaptureUi_ = false;
    VkBuffer       captureBuffer_ = VK_NULL_HANDLE;
    VmaAllocation  captureAlloc_  = VK_NULL_HANDLE;
    VkDeviceSize   captureBytes_  = 0;
    VkExtent2D     captureExtent_{};
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
    float pendingShadowBias_    = 0.001f;  // In 0..1 depth: about 0.09 m at a radius of 30.
    Mat4  pendingLightViewProj_ = Mat4::identity();
    // Cascaded sun shadows: on when a distance is set.
    static constexpr float kCascadeCasterReach = 60.0f;
    float          shadowCascadeDistance_ = 0.0f;
    bool           cascadesActive_ = false;
    ShadowCascades pendingCascades_{};
    // What each atlas tile holds: the near cascades are redrawn every frame, the far
    // two on alternate frames, so a tile's matrix is the one it was last drawn with.
    ShadowCascades renderedCascades_{};
    bool           renderedValid_ = false;
    Vec3           renderedSun_{0.0f, -1.0f, 0.0f};
    std::uint64_t  shadowFrame_ = 0;
    std::uint32_t  shadowRedraw_ = ~0u;
    Mat4           legacyLightViewProj_ = Mat4::identity();  // For shaders that know one light matrix.
    // The sun's shadow views this frame: the cascades, or the single bounded map.
    std::span<const Mat4> shadowViewsThisFrame() const {
        return cascadesActive_ ? std::span<const Mat4>(renderedCascades_.viewProj)
                               : std::span<const Mat4>(&pendingLightViewProj_, 1);
    }
    VkShadowMap shadowMap_;

    // M16 — cubemap storage + skybox subsystem + currently-set skybox.
    VkCubemapStore  cubemaps_;
    VkIblBaker      iblBaker_;
    VkSceneCapture  sceneCapture_;  // M49 — probe capture rig
    VkSkybox       skybox_;
    CubemapHandle  pendingSkybox_ = kInvalidHandle;
    CubemapHandle  pendingIrradiance_ = kInvalidHandle;  // M46b — baked from the skybox
    CubemapHandle  pendingPrefiltered_ = kInvalidHandle;  // M46c — baked from the skybox
    CubemapHandle  lastBakedSkybox_   = kInvalidHandle;  // M46b — bake-once guard

    // M49 — active reflection probes; set via setReflectionProbes.
    std::vector<GpuReflectionProbe> pendingProbes_;

    // M36 — offscreen scene-color target + post-process composite pipeline.
    VkPostProcess         postProcess_;
    EffectTable           effects_;   // M36 C3 — per-id effect styles; updated via setEffectStyle

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
    std::vector<std::function<void(VkCommandBuffer)>> deferredSceneCompute_;
    std::vector<std::function<void(VkCommandBuffer)>> deferredScenePass_;

    // M36/M43a -- UI/overlay callbacks recorded in the swapchain pass after the viewport blit.
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

    // Render-finished semaphores, one PER SWAPCHAIN IMAGE (not per
    // frame-in-flight). The submit signals and present waits on the
    // semaphore for the acquired image index. A per-frame semaphore is
    // unsafe here: with N images and 2 frames in flight, frame slot reuse
    // can recycle a semaphore an image is still presenting against, which
    // trips VUID-vkQueueSubmit-pSignalSemaphores-00067 and can DEVICE_LOST.
    std::vector<VkSemaphore> imageRenderFinished_;
    bool       pendingResize_  = false;
    int        pendingResizeWidth_ = 0;
    int        pendingResizeHeight_ = 0;
    bool       skipFrame_ = false;  // set when acquire fails this frame
    // Default true: blit the viewport image to the swapchain (games). The
    // editor sets false and composites the image via ImGui::Image instead.
    bool       blitViewportToSwapchain_ = true;
    float      pendingExposure_ = 1.0f;
    float      pendingBloomThreshold_ = 1.0f;
    float      pendingBloomKnee_      = 0.5f;
    float      pendingBloomIntensity_ = 0.05f;
    float      pendingBloomScatter_   = 0.85f;
    float      pendingSsaoRadius_   = 0.5f;
    float      pendingSsaoBias_     = 0.025f;
    float      pendingSsaoPower_    = 1.5f;
    float      pendingSsaoStrength_ = 1.0f;
    // M50b — wireframe toggle + tessellation factor for tessellated draws.
    bool       pendingWireframe_    = false;
    float      pendingTessFactor_   = 16.0f;
    // M50c — adaptive tessellation mode + target NDC edge length.
    bool       pendingTessAdaptive_    = false;
    float      pendingTessTargetEdge_  = 0.08f;
};

}  // namespace iron
