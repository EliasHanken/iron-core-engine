#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VkContext;
class VkSwapchain;
struct VkShader;

// Owns the foundation render pass + one VkPipeline per shader + framebuffers.
// Recreate framebuffers when the swapchain is recreated.
class VkPipeline {
public:
    bool init(VkContext& ctx, VkSwapchain& swap);
    void destroy(VkContext& ctx);

    // Recreate framebuffers only (render pass + pipeline stay valid).
    bool recreateFramebuffers(VkContext& ctx, VkSwapchain& swap);

    // Build (or fetch) a graphics pipeline for a given VkShader.
    // Cached by (shader pointer, wireframe).
    ::VkPipeline pipelineFor(VkContext& ctx, VkSwapchain& swap, const VkShader& sh,
                             bool wireframe = false);

    // M23 — build (or fetch) a graphics pipeline for a skinned shader.
    // Identical to pipelineFor() except the vertex input layout matches
    // SkinnedVertex (6 attributes, 76-byte stride). Cached separately.
    // M50b — wireframe param threads the polygon-mode flag into the skinned pipeline.
    ::VkPipeline skinnedPipelineFor(VkContext& ctx, VkSwapchain& swap,
                                     const VkShader& sh, bool wireframe = false);

    // M28 — drop the cached pipeline(s) built against `sh` (both the scene
    // and skinned caches are checked). The matching VkPipeline is destroyed;
    // the next pipelineFor/skinnedPipelineFor call rebuilds it. Caller must
    // ensure the device is idle (no in-flight use of the pipeline).
    void invalidate(VkContext& ctx, const VkShader* sh);

    VkRenderPass  renderPass()                    const { return renderPass_; }
    VkFramebuffer framebuffer(std::uint32_t i)    const { return framebuffers_[i]; }

    // Scene geometry records into VkPostProcess's scenePass_ (sampleable
    // offscreen target, dependencyCount=3) since M36 — NOT the swapchain
    // pass. Scene/skinned pipelines must be built against it for render-pass
    // compatibility (VUID-02684). Set once after post-process init, before
    // the first draw. renderPass_ stays the swapchain pass (framebuffers + UI).
    void setScenePass(VkRenderPass pass) { scenePass_ = pass; }

private:
    VkRenderPass               renderPass_   = VK_NULL_HANDLE;
    VkRenderPass               scenePass_    = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    // One pipeline per (shader pointer, wireframe) pair.
    struct PipelineEntry { const VkShader* shader; bool wireframe; ::VkPipeline pipeline; };
    std::vector<PipelineEntry> pipelines_;
    // M23/M50b — separate cache for the skinned vertex-input pipeline.
    // Keyed by (shader, wireframe) so wireframe toggle works for skinned draws too.
    std::vector<PipelineEntry> skinnedPipelines_;
};

}  // namespace iron
