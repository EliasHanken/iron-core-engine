#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkPostProcess is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "render/PostChainPlan.h"
#include "render/PostEffect.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

namespace iron {

class VkContext;

// Bloom fragment shader sources (exposed for compile-check tests; reuse the
// existing kFullscreenVert for the vertex stage).
const char* kBloomPrefilterDownSrc();  // sceneColor -> mip0 (threshold+knee+Karis)
const char* kBloomDownsampleSrc();     // mip[i] -> mip[i+1] (13-tap)
const char* kBloomUpsampleSrc();       // mip[i+1] -> mip[i] (3x3 tent, additive)

// Owns the offscreen render targets and full-screen passes for the M36
// post-process chain. Phase A: a single offscreen scene-color target (+ depth)
// matching the swapchain, and a "copy" pipeline that blits it to the swapchain
// image. Phase C adds a mask target (R8_UINT color + D32 depth) and a mask pass
// that re-renders tagged draws (effectId != 0) writing their effectId per pixel.
// Recreated on resize. Mirrors VkReflectionTarget.
class VkPostProcess {
public:
    // Push constants for the mask pipeline. Shared with VulkanRenderer so it
    // can fill and push these without duplicating the struct.
    struct MaskPushConstants {
        Mat4     mvp;  // 64 bytes — vertex stage: transform to clip space
        uint32_t id;   // 4 bytes  — fragment stage: effectId written to R8_UINT
    };

    bool init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
              VkExtent2D extent, VkSampler sharedSampler,
              VkRenderPass swapchainPass);
    void destroy(VkContext& ctx);
    bool resize(VkContext& ctx, VkExtent2D extent);

    VkRenderPass  scenePass()        const { return scenePass_; }
    // Pass that composite + debug + HUD overlays record into (M43a). Pipelines
    // recorded here must be built against it, not the swapchain pass — their
    // dependencyCount differs and that breaks render-pass compatibility.
    VkRenderPass  viewportPass()     const { return viewportPass_; }
    VkFramebuffer sceneFramebuffer() const { return sceneFb_; }
    VkExtent2D    extent()           const { return extent_; }

    void beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endScenePass(VkCommandBuffer cb) const;
    void recordComposite(VkCommandBuffer cb, float exposure) const;
    // Full-screen blit of viewportColor into the (already-begun) swapchain pass.
    void blitToSwapchain(VkCommandBuffer cb) const;

    VkImageView   viewportColorView() const { return viewportColorView_; }
    VkSampler     viewportSampler()   const { return sampler_; }
    VkExtent2D    viewportExtent()     const { return viewportExtent_; }

    // Begin/end the offscreen viewport pass (color cleared to clearColor,
    // depth cleared to 1.0). Composite + debug-lines + HUD record between these.
    void beginViewportPass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endViewportPass(VkCommandBuffer cb) const;

    // Resize ONLY the viewport target (scene/mask/glow targets unchanged).
    // No-op on unchanged/zero extent. Calls vkDeviceWaitIdle internally.
    bool resizeViewport(VkContext& ctx, VkExtent2D extent);

    // Run the post-process chain for this frame into the (already-begun) viewport
    // pass (M43a). `passes` from planPostChain(); `effects` supplies per-id styles.
    void runChain(VkCommandBuffer cb,
                  const std::vector<PostPass>& passes,
                  const EffectTable& effects,
                  VkExtent2D swapExtent,
                  float exposure);

    // --- Mask pass API (Phase C) ---
    VkRenderPass maskPass() const { return maskPass_; }

    // Begins the mask render pass: clears color to 0 (no effect), depth to 1.0,
    // sets the negative-height scene viewport + scissor so geometry orientation
    // matches the scene pass.
    void beginMaskPass(VkCommandBuffer cb) const;
    void endMaskPass(VkCommandBuffer cb) const;

    // Binds the mask pipeline. Call after beginMaskPass.
    void bindMaskPipeline(VkCommandBuffer cb) const;

    // Pipeline layout with push-constant range for MaskPushConstants. Exposed
    // so VulkanRenderer can call vkCmdPushConstants with the correct layout.
    VkPipelineLayout maskPipelineLayout() const { return maskPipeLayout_; }

    // Image views for the mask targets. Exposed so later passes (outline, x-ray)
    // can build descriptor sets sampling these images.
    VkImageView maskColorView() const { return maskColorView_; }
    VkImageView maskDepthView() const { return maskDepthView_; }

    // Scene depth view (full scene depth buffer, sampleable). Used by the x-ray
    // pass to compare against the masked object's own depth.
    VkImageView sceneDepthView() const { return sceneDepthView_; }

    // Push constants for the outline pipeline.
    struct OutlinePush {
        float color[4];   // rgb outline color, a unused
        float texel[2];   // 1/width, 1/height (of the mask/screen)
        float width;      // outline thickness in pixels
        float exposure;   // M44: tonemap exposure (was _pad)
    };

    // Push constants for the glow blur pipelines (H and V — direction is
    // implicit per-pipeline, not encoded in the push constant).
    struct GlowBlurPush {
        float texel[2];   // 1/width, 1/height
        float radius;     // blur radius in pixels (maps to style.width)
        float _pad;
    };

    // Push constants for the glow composite pipeline.
    struct GlowCompositePush {
        float color[4];      // rgb halo color + padding
        float intensity;     // halo strength (style.intensity)
        float exposure;      // M44: tonemap exposure (was _pad[0])
        float _pad[2];
    };

    // Push constants for the x-ray pipeline.
    struct XRayPush {
        float color[4];      // rgb tint color + padding
        float intensity;     // tint strength
        float exposure;      // M44: tonemap exposure (was _pad[0])
        float _pad[2];
    };

    // Push constants for the copy/composite (tonemap) pipeline.
    struct CopyPush {
        float exposure;   // linear exposure multiply applied before ACES
        float _pad[3];
    };

    // Run the offscreen pre-passes (GlowBlurH, GlowBlurV) that must execute
    // OUTSIDE any render pass. Called by VulkanRenderer::endFrame
    // BEFORE beginViewportPass. For Copy/Outline/XRay this is a no-op;
    // for GlowOutline it runs the two blur passes into glowFb_[0/1].
    void runChainOffscreenPasses(VkCommandBuffer cb,
                                 const std::vector<PostPass>& passes,
                                 const EffectTable& effects,
                                 VkExtent2D swapExtent);

private:
    VkContext* ctx_ = nullptr;
    VkSampler  sampler_     = VK_NULL_HANDLE;  // linear-repeat (from VkTextureStore)
    VkSampler  maskSampler_ = VK_NULL_HANDLE;  // NEAREST — required for integer (R8_UINT) textures
    VkExtent2D extent_{};
    VkFormat   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat   depthFormat_ = VK_FORMAT_UNDEFINED;
    // HDR linear-radiance format for the scene target (M44). Scene geometry +
    // skybox + particles render here; the composite step tone-maps it down to
    // the LDR `colorFormat_` viewportColor_. R16G16B16A16_SFLOAT is renderable,
    // blendable, and sampleable on all target GPUs (no feature flag needed).
    VkFormat   hdrFormat_   = VK_FORMAT_R16G16B16A16_SFLOAT;

    // --- M43a: final composited "viewport" target (color + depth). Sized
    // independently of the swapchain (defaults to swapchain extent). The
    // composite + debug-line + HUD overlays render here; the swapchain pass
    // then blits this image (and, later, ImGui samples it directly). ---
    VkExtent2D    viewportExtent_{};
    VkImage       viewportColor_      = VK_NULL_HANDLE;
    VmaAllocation viewportColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   viewportColorView_  = VK_NULL_HANDLE;
    VkImage       viewportDepth_      = VK_NULL_HANDLE;
    VmaAllocation viewportDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   viewportDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  viewportPass_       = VK_NULL_HANDLE;
    VkFramebuffer viewportFb_         = VK_NULL_HANDLE;

    // --- Scene offscreen target ---
    VkImage       sceneColor_      = VK_NULL_HANDLE;
    VmaAllocation sceneColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneColorView_  = VK_NULL_HANDLE;
    VkImage       sceneDepth_      = VK_NULL_HANDLE;
    VmaAllocation sceneDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  scenePass_       = VK_NULL_HANDLE;
    VkFramebuffer sceneFb_         = VK_NULL_HANDLE;

    // --- Mask target (R8_UINT color + D32 depth) ---
    VkImage       maskColor_      = VK_NULL_HANDLE;
    VmaAllocation maskColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   maskColorView_  = VK_NULL_HANDLE;
    VkImage       maskDepth_      = VK_NULL_HANDLE;
    VmaAllocation maskDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   maskDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  maskPass_       = VK_NULL_HANDLE;
    VkFramebuffer maskFb_         = VK_NULL_HANDLE;

    // --- Copy (composite) pipeline ---
    VkDescriptorSetLayout copySetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      copyPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          copyPipeline_   = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_       = VK_NULL_HANDLE;
    VkDescriptorSet       copyDescSet_    = VK_NULL_HANDLE;

    // --- M43a: viewport→swapchain blit (own copy pipeline + descriptor set,
    // built against the swapchain pass, sampling viewportColorView_). ---
    VkDescriptorSetLayout blitSetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      blitPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          blitPipeline_   = VK_NULL_HANDLE;
    VkDescriptorSet       blitDescSet_    = VK_NULL_HANDLE;

    // --- Outline pipeline ---
    VkDescriptorSetLayout outlineSetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      outlinePipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          outlinePipeline_   = VK_NULL_HANDLE;
    VkDescriptorSet       outlineDescSet_    = VK_NULL_HANDLE;

    // --- Mask pipeline (push-constant only, no descriptor sets) ---
    VkPipelineLayout maskPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline     maskPipeline_   = VK_NULL_HANDLE;

    // --- Glow scratch ping-pong targets (R16_SFLOAT coverage, 2 images) ---
    // H pass reads mask -> writes scratch[0]; V pass reads scratch[0] -> writes scratch[1].
    // This is the ping-pong realized for a single H/V blur pair (indices 0 and 1 fixed).
    VkImage       glowScratch_[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation glowScratchAlloc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView   glowScratchView_[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Single render pass for both scratch framebuffers (both use R16_SFLOAT).
    VkRenderPass  glowPass_            = VK_NULL_HANDLE;
    VkFramebuffer glowFb_[2]           = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // --- Glow pipelines ---
    VkDescriptorSetLayout glowBlurHSetLayout_     = VK_NULL_HANDLE;
    VkPipelineLayout      glowBlurHPipeLayout_    = VK_NULL_HANDLE;
    ::VkPipeline          glowBlurHPipeline_      = VK_NULL_HANDLE;
    VkDescriptorSet       glowBlurHDescSet_        = VK_NULL_HANDLE;

    VkDescriptorSetLayout glowBlurVSetLayout_     = VK_NULL_HANDLE;
    VkPipelineLayout      glowBlurVPipeLayout_    = VK_NULL_HANDLE;
    ::VkPipeline          glowBlurVPipeline_      = VK_NULL_HANDLE;
    VkDescriptorSet       glowBlurVDescSet_        = VK_NULL_HANDLE;

    VkDescriptorSetLayout glowCompositeSetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      glowCompositePipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          glowCompositePipeline_   = VK_NULL_HANDLE;
    VkDescriptorSet       glowCompositeDescSet_    = VK_NULL_HANDLE;

    // --- X-ray pipeline ---
    VkDescriptorSetLayout xraySetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      xrayPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          xrayPipeline_   = VK_NULL_HANDLE;
    VkDescriptorSet       xrayDescSet_    = VK_NULL_HANDLE;

    bool createTargets(VkContext& ctx);
    void destroyTargets(VkContext& ctx);
    bool createViewportTarget(VkContext& ctx);
    void destroyViewportTarget(VkContext& ctx);
    // Composite pipelines record into viewportPass_ — no swapchain pass needed.
    bool createCopyPipeline(VkContext& ctx);
    bool createBlitPipeline(VkContext& ctx, VkRenderPass swapchainPass);
    bool createOutlinePipeline(VkContext& ctx);
    bool createMaskPipeline(VkContext& ctx);
    bool createGlowPipelines(VkContext& ctx);
    bool createXRayPipeline(VkContext& ctx);
};

}  // namespace iron
