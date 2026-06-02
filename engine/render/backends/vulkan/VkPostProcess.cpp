// VkPostProcess.cpp — offscreen scene-color target + full-screen copy pipeline.

#include "render/backends/vulkan/VkPostProcess.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "scene/Mesh.h"
#include "core/Log.h"

namespace iron {

namespace {

const char* kFullscreenVert = R"(#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kCopyFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
void main() { outColor = texture(uScene, vUV); }
)";

// --- Mask pass shaders ---
// Position-only; MVP from push constant. The vertex buffer is the full Vertex
// struct (stride = sizeof(Vertex)) with position at location 0, mirroring
// VkShadowMap's vertex input setup so normal mesh vertex buffers bind directly.
const char* kMaskVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
void main() { gl_Position = pc.mvp * vec4(aPos, 1.0); }
)";

// Writes the effect id into the R8_UINT color attachment.
const char* kMaskFrag = R"(#version 450
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
layout(location = 0) out uint outId;
void main() { outId = pc.id; }
)";

// --- Outline pass shaders ---
// Full-screen triangle (vertex = kFullscreenVert). Samples scene-color (binding 0)
// and the R8_UINT mask id (binding 1). Edge-detects the mask with a 3x3 kernel
// and composites the outline color over the scene.
const char* kOutlineFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform usampler2D uMask;
layout(push_constant) uniform Push {
    vec4  color;
    vec2  texel;
    float width;
    float _pad;
} pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    uint here = texture(uMask, vUV).r;
    float edge = 0.0;
    for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0) continue;
        vec2 o = vec2(float(dx), float(dy)) * pc.texel * pc.width;
        uint n = texture(uMask, vUV + o).r;
        if (n != here) edge = 1.0;
    }
    outColor = vec4(mix(scene, pc.color.rgb, edge), 1.0);
}
)";

// --- Glow blur + composite shaders ---

// kGlowBlurHFrag — reads the R8_UINT mask, converts to coverage (0 or 1),
// then horizontally blurs it using a 13-tap Gaussian-ish kernel. Sampling
// the mask directly in the blur pass folds the coverage→float step in,
// preserving the 3-pass plan (GlowBlurH, GlowBlurV, GlowComposite).
const char* kGlowBlurHFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outCov;
layout(set = 0, binding = 0) uniform usampler2D uMask;
layout(push_constant) uniform Push { vec2 texel; float radius; float _pad; } pc;
void main() {
    float sum = 0.0, wsum = 0.0;
    for (int i = -6; i <= 6; ++i) {
        float w = exp(-float(i * i) / 18.0);
        vec2 o = vec2(float(i) * pc.radius / 6.0, 0.0) * pc.texel;
        float cov = texture(uMask, vUV + o).r > 0u ? 1.0 : 0.0;
        sum += cov * w; wsum += w;
    }
    outCov = sum / wsum;
}
)";

// kGlowBlurVFrag — reads blurred coverage from scratch[0] (sampler2D) and
// blurs it vertically into scratch[1].
const char* kGlowBlurVFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outCov;
layout(set = 0, binding = 0) uniform sampler2D uCov;
layout(push_constant) uniform Push { vec2 texel; float radius; float _pad; } pc;
void main() {
    float sum = 0.0, wsum = 0.0;
    for (int i = -6; i <= 6; ++i) {
        float w = exp(-float(i * i) / 18.0);
        vec2 o = vec2(0.0, float(i) * pc.radius / 6.0) * pc.texel;
        sum += texture(uCov, vUV + o).r * w; wsum += w;
    }
    outCov = sum / wsum;
}
)";

// kGlowCompositeFrag — adds a colored halo (blurred coverage minus solid
// interior) over the scene. Runs inside the viewport pass (M43a).
const char* kGlowCompositeFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBlur;
layout(set = 0, binding = 2) uniform usampler2D uMask;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float blur  = texture(uBlur, vUV).r;
    float solid = texture(uMask, vUV).r > 0u ? 1.0 : 0.0;
    float halo  = max(blur - solid, 0.0) * pc.intensity;
    outColor = vec4(scene + pc.color.rgb * halo, 1.0);
}
)";

// kXrayFrag — tint where the tagged object is occluded by nearer scene geometry.
// binding 0: sampler2D  uScene      — full scene color
// binding 1: usampler2D uMask       — tagged object id (0 = no object)
// binding 2: sampler2D  uMaskDepth  — tagged object's own depth (from mask pass)
// binding 3: sampler2D  uSceneDepth — full scene depth (includes occluders)
// Tints toward pc.color where id != 0 AND sceneDepth < maskDepth (occluded).
// Both depths use the same projection, so raw float comparison is correct;
// smaller depth value means nearer to the camera.
const char* kXrayFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform usampler2D uMask;
layout(set = 0, binding = 2) uniform sampler2D uMaskDepth;
layout(set = 0, binding = 3) uniform sampler2D uSceneDepth;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    uint id = texture(uMask, vUV).r;
    if (id == 0u) { outColor = vec4(scene, 1.0); return; }
    float md = texture(uMaskDepth, vUV).r;   // tagged object depth
    float sd = texture(uSceneDepth, vUV).r;  // nearest scene depth
    // Occluded when something is in front of the object (smaller depth = nearer).
    float occluded = (sd < md - 1e-4) ? 1.0 : 0.0;
    outColor = vec4(mix(scene, pc.color.rgb, occluded * pc.intensity), 1.0);
}
)";

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VkPostProcess::init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
                         VkExtent2D extent, VkSampler sharedSampler,
                         VkRenderPass swapchainPass) {
    ctx_             = &ctx;
    colorFormat_     = colorFormat;
    depthFormat_     = depthFormat;
    extent_          = extent;
    viewportExtent_  = extent;
    sampler_         = sharedSampler;

    // Create a NEAREST sampler for integer textures (R8_UINT mask).
    // The shared sampler_ from VkTextureStore is LINEAR; integer formats
    // cannot be linearly filtered (Vulkan validation error), so a dedicated
    // NEAREST sampler is required here.
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &maskSampler_));
    }

    if (!createTargets(ctx))                       { destroy(ctx); return false; }
    if (!createViewportTarget(ctx))                { destroy(ctx); return false; }
    if (!createCopyPipeline(ctx))                  { destroy(ctx); return false; }
    if (!createBlitPipeline(ctx, swapchainPass))   { destroy(ctx); return false; }
    if (!createOutlinePipeline(ctx))               { destroy(ctx); return false; }
    if (!createMaskPipeline(ctx))                  { destroy(ctx); return false; }
    if (!createGlowPipelines(ctx))                 { destroy(ctx); return false; }
    if (!createXRayPipeline(ctx))                  { destroy(ctx); return false; }
    return true;
}

void VkPostProcess::destroy(VkContext& ctx) {
    // X-ray pipeline objects.
    if (xrayPipeline_)   { vkDestroyPipeline(ctx.device(), xrayPipeline_, nullptr); xrayPipeline_ = VK_NULL_HANDLE; }
    if (xrayPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), xrayPipeLayout_, nullptr); xrayPipeLayout_ = VK_NULL_HANDLE; }
    if (xraySetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), xraySetLayout_, nullptr); xraySetLayout_ = VK_NULL_HANDLE; }
    xrayDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Mask pipeline objects (no descriptor set layout — push-constant only).
    if (maskPipeline_)   { vkDestroyPipeline(ctx.device(), maskPipeline_, nullptr); maskPipeline_ = VK_NULL_HANDLE; }
    if (maskPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), maskPipeLayout_, nullptr); maskPipeLayout_ = VK_NULL_HANDLE; }

    // Outline pipeline objects.
    if (outlinePipeline_)   { vkDestroyPipeline(ctx.device(), outlinePipeline_, nullptr); outlinePipeline_ = VK_NULL_HANDLE; }
    if (outlinePipeLayout_) { vkDestroyPipelineLayout(ctx.device(), outlinePipeLayout_, nullptr); outlinePipeLayout_ = VK_NULL_HANDLE; }
    if (outlineSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), outlineSetLayout_, nullptr); outlineSetLayout_ = VK_NULL_HANDLE; }
    outlineDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Glow pipeline objects.
    if (glowCompositePipeline_)   { vkDestroyPipeline(ctx.device(), glowCompositePipeline_, nullptr);   glowCompositePipeline_ = VK_NULL_HANDLE; }
    if (glowCompositePipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowCompositePipeLayout_, nullptr); glowCompositePipeLayout_ = VK_NULL_HANDLE; }
    if (glowCompositeSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowCompositeSetLayout_, nullptr); glowCompositeSetLayout_ = VK_NULL_HANDLE; }
    glowCompositeDescSet_ = VK_NULL_HANDLE;

    if (glowBlurVPipeline_)   { vkDestroyPipeline(ctx.device(), glowBlurVPipeline_, nullptr);   glowBlurVPipeline_ = VK_NULL_HANDLE; }
    if (glowBlurVPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowBlurVPipeLayout_, nullptr); glowBlurVPipeLayout_ = VK_NULL_HANDLE; }
    if (glowBlurVSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowBlurVSetLayout_, nullptr); glowBlurVSetLayout_ = VK_NULL_HANDLE; }
    glowBlurVDescSet_ = VK_NULL_HANDLE;

    if (glowBlurHPipeline_)   { vkDestroyPipeline(ctx.device(), glowBlurHPipeline_, nullptr);   glowBlurHPipeline_ = VK_NULL_HANDLE; }
    if (glowBlurHPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowBlurHPipeLayout_, nullptr); glowBlurHPipeLayout_ = VK_NULL_HANDLE; }
    if (glowBlurHSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowBlurHSetLayout_, nullptr); glowBlurHSetLayout_ = VK_NULL_HANDLE; }
    glowBlurHDescSet_ = VK_NULL_HANDLE;

    // Blit (viewport→swapchain) pipeline objects.
    if (blitPipeline_)   { vkDestroyPipeline(ctx.device(), blitPipeline_, nullptr); blitPipeline_ = VK_NULL_HANDLE; }
    if (blitPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), blitPipeLayout_, nullptr); blitPipeLayout_ = VK_NULL_HANDLE; }
    if (blitSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), blitSetLayout_, nullptr); blitSetLayout_ = VK_NULL_HANDLE; }
    blitDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Copy (composite) pipeline objects.
    if (copyPipeline_)   { vkDestroyPipeline(ctx.device(), copyPipeline_, nullptr); copyPipeline_ = VK_NULL_HANDLE; }
    if (copyPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), copyPipeLayout_, nullptr); copyPipeLayout_ = VK_NULL_HANDLE; }
    if (copySetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), copySetLayout_, nullptr); copySetLayout_ = VK_NULL_HANDLE; }
    // descPool_ owns all descriptor sets; destroying the pool frees them all.
    if (descPool_)       { vkDestroyDescriptorPool(ctx.device(), descPool_, nullptr); descPool_ = VK_NULL_HANDLE; copyDescSet_ = VK_NULL_HANDLE; }

    // NEAREST sampler for integer mask texture.
    if (maskSampler_)    { vkDestroySampler(ctx.device(), maskSampler_, nullptr); maskSampler_ = VK_NULL_HANDLE; }

    // Render target objects (images + framebuffers). The render passes are
    // persistent across resize, so they are NOT freed by these helpers —
    // destroy them here, at full teardown.
    destroyViewportTarget(ctx);
    destroyTargets(ctx);
    if (viewportPass_) { vkDestroyRenderPass(ctx.device(), viewportPass_, nullptr); viewportPass_ = VK_NULL_HANDLE; }
    if (glowPass_)     { vkDestroyRenderPass(ctx.device(), glowPass_, nullptr);     glowPass_ = VK_NULL_HANDLE; }
    if (maskPass_)     { vkDestroyRenderPass(ctx.device(), maskPass_, nullptr);     maskPass_ = VK_NULL_HANDLE; }
    if (scenePass_)    { vkDestroyRenderPass(ctx.device(), scenePass_, nullptr);    scenePass_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::resize(VkContext& ctx, VkExtent2D extent) {
    extent_ = extent;
    destroyTargets(ctx);
    if (!createTargets(ctx)) return false;

    // Re-write the copy descriptor set to point at the new sceneColorView_.
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = sceneColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = copyDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }

    // Re-write the outline descriptor set to point at the new views.
    {
        VkDescriptorImageInfo imgInfos[2]{};
        imgInfos[0].sampler     = sampler_;
        imgInfos[0].imageView   = sceneColorView_;
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = maskSampler_;
        imgInfos[1].imageView   = maskColorView_;
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = outlineDescSet_;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &imgInfos[0];
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = outlineDescSet_;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &imgInfos[1];
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
    }

    // Re-write glow descriptor sets to point at the new size-varying views.
    // GlowBlurH: binding 0 = maskColorView_ (NEAREST sampler).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = maskSampler_;
        imgInfo.imageView   = maskColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = glowBlurHDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowBlurV: binding 0 = glowScratchView_[0] (linear sampler).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = glowScratchView_[0];
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = glowBlurVDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowComposite: binding 0 = sceneColorView_, binding 1 = glowScratchView_[1], binding 2 = maskColorView_.
    {
        VkDescriptorImageInfo imgInfos[3]{};
        imgInfos[0].sampler     = sampler_;
        imgInfos[0].imageView   = sceneColorView_;
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = sampler_;
        imgInfos[1].imageView   = glowScratchView_[1];
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[2].sampler     = maskSampler_;
        imgInfos[2].imageView   = maskColorView_;
        imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = glowCompositeDescSet_;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);
    }

    // XRay: binding 0 = sceneColorView_, binding 1 = maskColorView_,
    //        binding 2 = maskDepthView_, binding 3 = sceneDepthView_.
    {
        VkDescriptorImageInfo imgInfos[4]{};
        imgInfos[0].sampler     = sampler_;
        imgInfos[0].imageView   = sceneColorView_;
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = maskSampler_;
        imgInfos[1].imageView   = maskColorView_;
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[2].sampler     = maskSampler_;
        imgInfos[2].imageView   = maskDepthView_;
        imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imgInfos[3].sampler     = maskSampler_;
        imgInfos[3].imageView   = sceneDepthView_;
        imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[4]{};
        for (int i = 0; i < 4; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = xrayDescSet_;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 4, writes, 0, nullptr);
    }

    if (!resizeViewport(ctx, extent)) return false;

    return true;
}

void VkPostProcess::beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = scenePass_;
    rpBegin.framebuffer       = sceneFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = extent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = static_cast<float>(extent_.width);
    vp.height   = static_cast<float>(extent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endScenePass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::beginViewportPass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = viewportPass_;
    rpBegin.framebuffer       = viewportFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = viewportExtent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Plain positive-height viewport covering the full viewport extent.
    // Composite draws are UV-space full-screen quads; debug/HUD passes
    // set their own viewport after this.
    VkViewport vp{};
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = static_cast<float>(viewportExtent_.width);
    vp.height   = static_cast<float>(viewportExtent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, viewportExtent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endViewportPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::recordComposite(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copyPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            copyPipeLayout_, 0, 1, &copyDescSet_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

void VkPostProcess::blitToSwapchain(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            blitPipeLayout_, 0, 1, &blitDescSet_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

void VkPostProcess::beginMaskPass(VkCommandBuffer cb) const {
    // Clear color to 0 (no effect id), depth to 1.0 (far plane).
    VkClearValue clears[2]{};
    clears[0].color.uint32[0] = 0;
    clears[0].color.uint32[1] = 0;
    clears[0].color.uint32[2] = 0;
    clears[0].color.uint32[3] = 0;
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = maskPass_;
    rpBegin.framebuffer       = maskFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = extent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport so GL-style projection matrices render
    // right-side-up, consistent with the scene pass (VulkanRenderer::setSceneViewport).
    VkViewport vp{};
    vp.x        = 0;
    vp.y        = static_cast<float>(extent_.height);
    vp.width    = static_cast<float>(extent_.width);
    vp.height   = -static_cast<float>(extent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endMaskPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::bindMaskPipeline(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, maskPipeline_);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool VkPostProcess::createTargets(VkContext& ctx) {
    // --- Scene color image (hdrFormat_, COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = hdrFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneColor_, &sceneColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = hdrFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneColorView_));
    }

    // --- Scene depth image (depthFormat_, DEPTH_STENCIL_ATTACHMENT | SAMPLED) ---
    // SAMPLED_BIT is required so the x-ray pass can compare scene depth against
    // the tagged object's own (mask) depth. The render pass transitions depth to
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL at the end so the x-ray shader can sample it.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = depthFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneDepth_, &sceneDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = depthFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneDepthView_));
    }

    // --- Scene render pass: color + depth ---
    // Color final layout = SHADER_READ_ONLY_OPTIMAL so the composite pass
    // can sample it. Entry dep: FRAGMENT_SHADER -> COLOR_ATTACHMENT_OUTPUT
    // guards frame N+1 overwriting the image while frame N still samples it.
    // Exit dep: COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER lets the composite
    // pass sample safely.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format         = hdrFormat_;  // was colorFormat_ (scenePass_ color attachment)
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format         = depthFormat_;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  // must store so x-ray can sample it
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // sampleable for x-ray

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[3]{};
    // Entry: wait for prior frame's sampling to finish before writing color + depth.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    // Depth is written at LATE_FRAGMENT_TESTS; cover it so the
    // DEPTH_STENCIL_ATTACHMENT_WRITE access has a matching stage.
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Exit (color): composite pass can sample color after writes complete.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // Exit (depth): x-ray pass can sample depth after depth writes complete.
    deps[2].srcSubpass    = 0;
    deps[2].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[2].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[2].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 3;
    rpInfo.pDependencies   = deps;
    // Render passes depend only on formats, not extent — create once and keep
    // across resize. Pipelines are built against this handle; recreating it on
    // resize would invalidate them (VUID-02684 / DEVICE_LOST). Destroyed in
    // destroy(), not destroyTargets().
    if (scenePass_ == VK_NULL_HANDLE)
        VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &scenePass_));

    // --- Scene framebuffer ---
    {
        VkImageView views[2] = {sceneColorView_, sceneDepthView_};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = scenePass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = views;
        fbInfo.width           = extent_.width;
        fbInfo.height          = extent_.height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &sceneFb_));
    }

    // -----------------------------------------------------------------------
    // Mask target: R8_UINT color + D32_SFLOAT depth.
    // Both have SAMPLED usage so later passes (outline, x-ray) can sample them.
    // -----------------------------------------------------------------------

    // --- Mask color image (R8_UINT, COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R8_UINT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &maskColor_, &maskColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = maskColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_R8_UINT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &maskColorView_));
    }

    // --- Mask depth image (D32_SFLOAT, DEPTH_STENCIL_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_D32_SFLOAT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &maskDepth_, &maskDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = maskDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_D32_SFLOAT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &maskDepthView_));
    }

    // --- Mask render pass: R8_UINT color + D32 depth ---
    // Color finalLayout = SHADER_READ_ONLY_OPTIMAL so outline/glow can sample.
    // Depth finalLayout = DEPTH_STENCIL_READ_ONLY_OPTIMAL so x-ray can sample.
    // Subpass dependencies mirror VkReflectionTarget / scenePass_.
    {
        VkAttachmentDescription maskAttachments[2]{};
        // Color attachment (R8_UINT effectId).
        maskAttachments[0].format         = VK_FORMAT_R8_UINT;
        maskAttachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        maskAttachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        maskAttachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        maskAttachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        maskAttachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        maskAttachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        maskAttachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Depth attachment (D32_SFLOAT, own depth buffer for tagged objects).
        maskAttachments[1].format         = VK_FORMAT_D32_SFLOAT;
        maskAttachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        maskAttachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        maskAttachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        maskAttachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        maskAttachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        maskAttachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        maskAttachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference maskColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference maskDepthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription maskSubpass{};
        maskSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        maskSubpass.colorAttachmentCount    = 1;
        maskSubpass.pColorAttachments       = &maskColorRef;
        maskSubpass.pDepthStencilAttachment = &maskDepthRef;

        // Entry: wait for prior frame's fragment shader reads to finish
        // before this pass overwrites the color attachment.
        // Exit: let subsequent passes sample the color + depth results.
        VkSubpassDependency maskDeps[2]{};
        maskDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        maskDeps[0].dstSubpass    = 0;
        maskDeps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        maskDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        maskDeps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        // Depth is written at LATE_FRAGMENT_TESTS; cover it in the dst stage so the
        // DEPTH_STENCIL_ATTACHMENT_WRITE access has a matching stage.
        maskDeps[0].dstStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        maskDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        maskDeps[1].srcSubpass    = 0;
        maskDeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        maskDeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        maskDeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        maskDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        maskDeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        // No BY_REGION: outline shader reads a 3x3 mask neighborhood which can
        // cross framebuffer tiles on tiled GPUs.

        VkRenderPassCreateInfo maskRpInfo{};
        maskRpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        maskRpInfo.attachmentCount = 2;
        maskRpInfo.pAttachments    = maskAttachments;
        maskRpInfo.subpassCount    = 1;
        maskRpInfo.pSubpasses      = &maskSubpass;
        maskRpInfo.dependencyCount = 2;
        maskRpInfo.pDependencies   = maskDeps;
        if (maskPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &maskRpInfo, nullptr, &maskPass_));

        // --- Mask framebuffer ---
        VkImageView maskViews[2] = {maskColorView_, maskDepthView_};
        VkFramebufferCreateInfo maskFbInfo{};
        maskFbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        maskFbInfo.renderPass      = maskPass_;
        maskFbInfo.attachmentCount = 2;
        maskFbInfo.pAttachments    = maskViews;
        maskFbInfo.width           = extent_.width;
        maskFbInfo.height          = extent_.height;
        maskFbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &maskFbInfo, nullptr, &maskFb_));
    }

    // -----------------------------------------------------------------------
    // Glow ping-pong scratch targets: two R16_SFLOAT images for separable blur.
    // H pass writes scratch[0]; V pass reads scratch[0] and writes scratch[1].
    // R16_SFLOAT is widely supported as a color attachment + sampled format and
    // is the natural choice for single-channel float coverage in [0,1].
    // -----------------------------------------------------------------------
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R16_SFLOAT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &glowScratch_[i], &glowScratchAlloc_[i], nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = glowScratch_[i];
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_R16_SFLOAT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &glowScratchView_[i]));
    }

    // --- Glow render pass: single R16_SFLOAT color attachment ---
    // loadOp = DONT_CARE (every pixel is overwritten by the full-screen triangle).
    // finalLayout = SHADER_READ_ONLY_OPTIMAL so the next pass can sample it.
    // Subpass dependencies mirror maskPass_: entry waits for prior sampling
    // to finish before writing; exit lets the next pass sample safely.
    {
        VkAttachmentDescription glowAttachment{};
        glowAttachment.format         = VK_FORMAT_R16_SFLOAT;
        glowAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        glowAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        glowAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        glowAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        glowAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        glowAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        glowAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference glowColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription glowSubpass{};
        glowSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        glowSubpass.colorAttachmentCount = 1;
        glowSubpass.pColorAttachments    = &glowColorRef;

        VkSubpassDependency glowDeps[2]{};
        // Entry: wait for prior frame's sampling before writing color.
        glowDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        glowDeps[0].dstSubpass    = 0;
        glowDeps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        glowDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        glowDeps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        glowDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Exit: next pass can sample after color writes complete.
        glowDeps[1].srcSubpass      = 0;
        glowDeps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        glowDeps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        glowDeps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        glowDeps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        glowDeps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        // No BY_REGION: glow V blur samples y-offset taps on scratch[0] which
        // cross framebuffer tiles; glow composite samples blurred scratch[1].

        VkRenderPassCreateInfo glowRpInfo{};
        glowRpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        glowRpInfo.attachmentCount = 1;
        glowRpInfo.pAttachments    = &glowAttachment;
        glowRpInfo.subpassCount    = 1;
        glowRpInfo.pSubpasses      = &glowSubpass;
        glowRpInfo.dependencyCount = 2;
        glowRpInfo.pDependencies   = glowDeps;
        if (glowPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &glowRpInfo, nullptr, &glowPass_));

        // Two framebuffers — one per scratch image — both use the same glowPass_.
        for (int i = 0; i < 2; ++i) {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = glowPass_;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &glowScratchView_[i];
            fbInfo.width           = extent_.width;
            fbInfo.height          = extent_.height;
            fbInfo.layers          = 1;
            VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &glowFb_[i]));
        }
    }

    return true;
}

void VkPostProcess::destroyTargets(VkContext& ctx) {
    // Glow scratch targets (destroy in reverse creation order).
    // NOTE: glowPass_/maskPass_/scenePass_ are NOT destroyed here — render
    // passes are extent-independent and must persist across resize so the
    // pipelines built against them stay valid. They are torn down in destroy().
    for (int i = 1; i >= 0; --i) {
        if (glowFb_[i])          { vkDestroyFramebuffer(ctx.device(), glowFb_[i], nullptr); glowFb_[i] = VK_NULL_HANDLE; }
    }
    for (int i = 1; i >= 0; --i) {
        if (glowScratchView_[i]) { vkDestroyImageView(ctx.device(), glowScratchView_[i], nullptr); glowScratchView_[i] = VK_NULL_HANDLE; }
        if (glowScratch_[i])     { vmaDestroyImage(ctx.allocator(), glowScratch_[i], glowScratchAlloc_[i]); glowScratch_[i] = VK_NULL_HANDLE; glowScratchAlloc_[i] = VK_NULL_HANDLE; }
    }

    // Mask target (destroy before scene target — order doesn't matter to Vulkan,
    // but mirrors creation order in reverse for clarity).
    if (maskFb_)          { vkDestroyFramebuffer(ctx.device(), maskFb_, nullptr); maskFb_ = VK_NULL_HANDLE; }
    if (maskColorView_)   { vkDestroyImageView(ctx.device(), maskColorView_, nullptr); maskColorView_ = VK_NULL_HANDLE; }
    if (maskDepthView_)   { vkDestroyImageView(ctx.device(), maskDepthView_, nullptr); maskDepthView_ = VK_NULL_HANDLE; }
    if (maskColor_)       { vmaDestroyImage(ctx.allocator(), maskColor_, maskColorAlloc_); maskColor_ = VK_NULL_HANDLE; maskColorAlloc_ = VK_NULL_HANDLE; }
    if (maskDepth_)       { vmaDestroyImage(ctx.allocator(), maskDepth_, maskDepthAlloc_); maskDepth_ = VK_NULL_HANDLE; maskDepthAlloc_ = VK_NULL_HANDLE; }

    // Scene offscreen target.
    if (sceneFb_)         { vkDestroyFramebuffer(ctx.device(), sceneFb_, nullptr); sceneFb_ = VK_NULL_HANDLE; }
    if (sceneColorView_)  { vkDestroyImageView(ctx.device(), sceneColorView_, nullptr); sceneColorView_ = VK_NULL_HANDLE; }
    if (sceneDepthView_)  { vkDestroyImageView(ctx.device(), sceneDepthView_, nullptr); sceneDepthView_ = VK_NULL_HANDLE; }
    if (sceneColor_)      { vmaDestroyImage(ctx.allocator(), sceneColor_, sceneColorAlloc_); sceneColor_ = VK_NULL_HANDLE; sceneColorAlloc_ = VK_NULL_HANDLE; }
    if (sceneDepth_)      { vmaDestroyImage(ctx.allocator(), sceneDepth_, sceneDepthAlloc_); sceneDepth_ = VK_NULL_HANDLE; sceneDepthAlloc_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::createViewportTarget(VkContext& ctx) {
    // --- Viewport color image (colorFormat_, COLOR_ATTACHMENT | SAMPLED) ---
    // Uses the same format as the swapchain color so pipelines built against
    // the swapchain render pass are format-compatible with viewportPass_.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = colorFormat_;
        iInfo.extent        = {viewportExtent_.width, viewportExtent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &viewportColor_, &viewportColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = viewportColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = colorFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &viewportColorView_));
    }

    // --- Viewport depth image (depthFormat_, DEPTH_STENCIL_ATTACHMENT only) ---
    // No SAMPLED_BIT — nothing samples viewport depth; the attachment is
    // present only for depth-tested draws (debug lines, HUD) recording into this pass.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = depthFormat_;
        iInfo.extent        = {viewportExtent_.width, viewportExtent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &viewportDepth_, &viewportDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = viewportDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = depthFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &viewportDepthView_));
    }

    // --- Viewport render pass: color + depth, format-compatible with swapchain ---
    // Color finalLayout = SHADER_READ_ONLY_OPTIMAL so the swapchain blit pass
    // (Task 2) can sample it as a texture. Depth is DONT_CARE on store since
    // nothing samples viewport depth.
    // Two subpass dependencies (entry + color exit), mirroring the scene pass's
    // first two deps (the scene pass adds a third for sampleable depth — not
    // needed here since viewport depth is not sampled).
    {
        VkAttachmentDescription attachments[2]{};
        // Color attachment: swapchain color format, cleared + stored, transitions
        // to SHADER_READ_ONLY_OPTIMAL so the blit pipeline can sample it.
        attachments[0].format         = colorFormat_;
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Depth attachment: swapchain depth format, cleared, not stored (not sampled).
        attachments[1].format         = depthFormat_;
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
        // Entry: wait for prior frame's sampling to finish before writing color + depth.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Exit (color): blit/sample pass can read color after color writes complete.
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
        if (viewportPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &viewportPass_));

        // --- Viewport framebuffer ---
        VkImageView views[2] = {viewportColorView_, viewportDepthView_};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = viewportPass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = views;
        fbInfo.width           = viewportExtent_.width;
        fbInfo.height          = viewportExtent_.height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &viewportFb_));
    }

    return true;
}

void VkPostProcess::destroyViewportTarget(VkContext& ctx) {
    // viewportPass_ is NOT destroyed here — it persists across resize so the
    // composite/debug/HUD pipelines built against it stay valid (torn down in
    // destroy()).
    if (viewportFb_)         { vkDestroyFramebuffer(ctx.device(), viewportFb_, nullptr); viewportFb_ = VK_NULL_HANDLE; }
    if (viewportColorView_)  { vkDestroyImageView(ctx.device(), viewportColorView_, nullptr); viewportColorView_ = VK_NULL_HANDLE; }
    if (viewportDepthView_)  { vkDestroyImageView(ctx.device(), viewportDepthView_, nullptr); viewportDepthView_ = VK_NULL_HANDLE; }
    if (viewportColor_)      { vmaDestroyImage(ctx.allocator(), viewportColor_, viewportColorAlloc_); viewportColor_ = VK_NULL_HANDLE; viewportColorAlloc_ = VK_NULL_HANDLE; }
    if (viewportDepth_)      { vmaDestroyImage(ctx.allocator(), viewportDepth_, viewportDepthAlloc_); viewportDepth_ = VK_NULL_HANDLE; viewportDepthAlloc_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::resizeViewport(VkContext& ctx, VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return true;
    if (extent.width == viewportExtent_.width &&
        extent.height == viewportExtent_.height) return true;
    vkDeviceWaitIdle(ctx.device());
    destroyViewportTarget(ctx);
    viewportExtent_ = extent;
    if (!createViewportTarget(ctx)) return false;

    // Re-point the blit descriptor at the new viewport image.
    if (blitDescSet_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = viewportColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = blitDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    return true;
}

bool VkPostProcess::createCopyPipeline(VkContext& ctx) {
    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCopyFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler (FS).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &copySetLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &copySetLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &copyPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings or attributes — positions come from gl_VertexIndex.

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = copyPipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult copyPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &copyPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(copyPipeResult);

    // Descriptor pool: 7 sets total, 13 combined-image-sampler descriptors.
    //   set 0: copy         — 1 sampler  (scene color)
    //   set 1: blit         — 1 sampler  (viewport color)
    //   set 2: outline      — 2 samplers (scene color + mask)
    //   set 3: glowBlurH    — 1 sampler  (mask)
    //   set 4: glowBlurV    — 1 sampler  (scratch[0])
    //   set 5: glowComposite — 3 samplers (scene color + scratch[1] + mask)
    //   set 6: xray         — 4 samplers (scene color + mask id + mask depth + scene depth)
    // Total: 7 sets, 13 combined-image-samplers.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 13;

    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 7;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &descPool_));

    // Allocate and write the copy descriptor set.
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &copySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &copyDescSet_));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler_;
    imgInfo.imageView   = sceneColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = copyDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

bool VkPostProcess::createBlitPipeline(VkContext& ctx, VkRenderPass swapchainPass) {
    // Compile shaders (same kFullscreenVert + kCopyFrag as the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCopyFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: blit shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: blit fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler (FS).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &blitSetLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &blitSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &blitPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings or attributes — positions come from gl_VertexIndex.

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = blitPipeLayout_;
    pInfo.renderPass          = swapchainPass;
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult blitPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &blitPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(blitPipeResult);

    // Allocate and write the blit descriptor set (from the shared descPool_).
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &blitSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &blitDescSet_));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler_;
    imgInfo.imageView   = viewportColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = blitDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

bool VkPostProcess::createMaskPipeline(VkContext& ctx) {
    // Push-constant range: one range covering both stages (vertex uses mvp,
    // fragment uses id). Size = sizeof(MaskPushConstants) = 64 + 4 = 68 bytes.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(MaskPushConstants));

    // No descriptor sets — push-constant only.
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 0;
    plInfo.pSetLayouts            = nullptr;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &maskPipeLayout_));

    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kMaskVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kMaskFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: mask shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: mask fragment shader module creation failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    // Vertex input: full Vertex struct stride (44 bytes: Vec3+Vec3+Vec2+Vec3),
    // only position (location 0) declared — mirrors VkShadowMap exactly so
    // binding the normal mesh vertex buffer works without re-uploading.
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = offsetof(Vertex, uv);
    attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
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

    // Single R8_UINT color attachment — no blending (integers don't blend).
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = maskPipeLayout_;
    pInfo.renderPass          = maskPass_;
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult maskPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &maskPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(maskPipeResult);

    return true;
}

bool VkPostProcess::createOutlinePipeline(VkContext& ctx) {
    // Compile shaders (reuse kFullscreenVert from the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kOutlineFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: outline shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: outline fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = sampler2D (scene color), binding 1 = usampler2D (mask id).
    // Both are COMBINED_IMAGE_SAMPLER; the mask uses maskSampler_ (NEAREST) at runtime.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &outlineSetLayout_));

    // Push constant for outline params (fragment stage).
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(OutlinePush));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &outlineSetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &outlinePipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings — positions come from gl_VertexIndex.

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = outlinePipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult outlinePipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                                           1, &pInfo, nullptr,
                                                           &outlinePipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(outlinePipeResult);

    // Allocate the outline descriptor set from the shared pool (allocated in
    // createCopyPipeline with maxSets=6, descriptorCount=12).
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &outlineSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &outlineDescSet_));

    // Write the outline descriptor set: binding 0 = scene color (linear sampler),
    // binding 1 = mask id (NEAREST sampler — integer texture cannot be linear-filtered).
    VkDescriptorImageInfo imgInfos[2]{};
    imgInfos[0].sampler     = sampler_;
    imgInfos[0].imageView   = sceneColorView_;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = maskSampler_;
    imgInfos[1].imageView   = maskColorView_;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = outlineDescSet_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &imgInfos[0];
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = outlineDescSet_;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &imgInfos[1];
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    return true;
}

bool VkPostProcess::createGlowPipelines(VkContext& ctx) {
    // Compile the vertex shader (shared kFullscreenVert) once; reuse for all three passes.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kFullscreenVert);
    if (vspv.empty()) {
        Log::error("VkPostProcess: glow vertex shader compile failed");
        return false;
    }

    // Helper lambda: create a VkShaderModule from a SPIR-V vector.
    auto makeModule = [&](const std::vector<std::uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo smInfo{};
        smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = spv.size() * sizeof(std::uint32_t);
        smInfo.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
        return m;
    };

    // Common full-screen pipeline state (reused for all three passes).
    // We fill pInfo per-pipeline; only renderPass and layout differ.
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0xF;
    blendAtt.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings — positions come from gl_VertexIndex.

    // Push-constant range used by all glow passes (fragment stage only).
    // GlowBlurPush (16 bytes) and GlowCompositePush (32 bytes) — declare
    // the larger so both fit within one layout's range.
    VkPushConstantRange blurPcRange{};
    blurPcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurPcRange.offset     = 0;
    blurPcRange.size       = static_cast<uint32_t>(sizeof(GlowBlurPush));

    VkPushConstantRange compositePcRange{};
    compositePcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compositePcRange.offset     = 0;
    compositePcRange.size       = static_cast<uint32_t>(sizeof(GlowCompositePush));

    // -----------------------------------------------------------------
    // GlowBlurH pipeline: reads mask (usampler2D, NEAREST) -> writes R16_SFLOAT scratch[0].
    // Descriptor set layout: binding 0 = usampler2D uMask.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowBlurHSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowBlurHSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &blurPcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowBlurHPipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowBlurHFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow blur-H shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow blur-H shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        // BlurH writes a single R16_SFLOAT output — single-channel blend state.
        VkPipelineColorBlendAttachmentState singleAtt{};
        singleAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        singleAtt.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo singleBlend{};
        singleBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        singleBlend.attachmentCount = 1;
        singleBlend.pAttachments    = &singleAtt;

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &singleBlend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowBlurHPipeLayout_;
        pInfo.renderPass          = glowPass_;  // offscreen R16_SFLOAT pass
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowBlurHPipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // GlowBlurV pipeline: reads scratch[0] (sampler2D, linear) -> writes scratch[1].
    // Descriptor set layout: binding 0 = sampler2D uCov.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowBlurVSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowBlurVSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &blurPcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowBlurVPipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowBlurVFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow blur-V shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow blur-V shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkPipelineColorBlendAttachmentState singleAtt{};
        singleAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        singleAtt.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo singleBlend{};
        singleBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        singleBlend.attachmentCount = 1;
        singleBlend.pAttachments    = &singleAtt;

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &singleBlend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowBlurVPipeLayout_;
        pInfo.renderPass          = glowPass_;  // offscreen R16_SFLOAT pass
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowBlurVPipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // GlowComposite pipeline: reads scene + scratch[1] + mask; writes viewport pass
    // (M43a). Built against swapchainPass for render-pass compatibility.
    // Descriptor set layout: binding 0 = sampler2D uScene,
    //                        binding 1 = sampler2D uBlur,
    //                        binding 2 = usampler2D uMask.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2; bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[2].descriptorCount = 1; bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings    = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowCompositeSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowCompositeSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &compositePcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowCompositePipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowCompositeFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow composite shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow composite shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &blend;  // 4-channel RGBA for swapchain-compatible format
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowCompositePipeLayout_;
        pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_ (must match its dependencyCount)
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowCompositePipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // Allocate the three glow descriptor sets from the shared pool.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayout layouts[3] = {
            glowBlurHSetLayout_,
            glowBlurVSetLayout_,
            glowCompositeSetLayout_,
        };
        VkDescriptorSet sets[3] = {};
        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool     = descPool_;
        dsAlloc.descriptorSetCount = 3;
        dsAlloc.pSetLayouts        = layouts;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, sets));
        glowBlurHDescSet_     = sets[0];
        glowBlurVDescSet_     = sets[1];
        glowCompositeDescSet_ = sets[2];
    }

    // Write initial descriptor values (same views as resize would write).
    // GlowBlurH: binding 0 = maskColorView_ + maskSampler_ (NEAREST — integer texture).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = maskSampler_;
        imgInfo.imageView   = maskColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = glowBlurHDescSet_; write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1; write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowBlurV: binding 0 = glowScratchView_[0] + sampler_ (linear).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = glowScratchView_[0];
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = glowBlurVDescSet_; write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1; write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowComposite: binding 0 = sceneColorView_, binding 1 = scratch[1], binding 2 = maskColorView_.
    {
        VkDescriptorImageInfo imgInfos[3]{};
        imgInfos[0].sampler = sampler_;     imgInfos[0].imageView = sceneColorView_;    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler = sampler_;     imgInfos[1].imageView = glowScratchView_[1]; imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[2].sampler = maskSampler_; imgInfos[2].imageView = maskColorView_;     imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = glowCompositeDescSet_; writes[i].dstBinding = static_cast<uint32_t>(i);
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1; writes[i].pImageInfo = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);
    }

    return true;
}

void VkPostProcess::runChain(VkCommandBuffer cb,
                             const std::vector<PostPass>& passes,
                             const EffectTable& effects,
                             VkExtent2D swapExtent) {
    for (const PostPass pass : passes) {
        switch (pass) {
            case PostPass::Copy:
                recordComposite(cb);
                break;

            case PostPass::Outline: {
                // Find the first active Outline style in the effect table.
                const EffectStyle* os = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::Outline) {
                        os = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                OutlinePush pc{};
                if (os) {
                    pc.color[0] = os->color.x;
                    pc.color[1] = os->color.y;
                    pc.color[2] = os->color.z;
                    pc.color[3] = 1.0f;
                    pc.width    = os->width;
                } else {
                    // Fallback defaults (shouldn't happen if Outline is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.width    = 2.0f;
                }
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;
                pc._pad     = 0.0f;

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        outlinePipeLayout_, 0, 1, &outlineDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, outlinePipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(OutlinePush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            case PostPass::GlowComposite: {
                // GlowBlurH and GlowBlurV ran in runChainOffscreenPasses (before
                // beginViewportPass). Here we composite the blurred coverage over
                // the scene, writing into the viewport pass (which the caller has begun).
                const EffectStyle* gs = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::GlowOutline) {
                        gs = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                GlowCompositePush pc{};
                if (gs) {
                    pc.color[0]  = gs->color.x;
                    pc.color[1]  = gs->color.y;
                    pc.color[2]  = gs->color.z;
                    pc.color[3]  = 1.0f;
                    pc.intensity = gs->intensity;
                } else {
                    // Fallback defaults (shouldn't happen if GlowOutline is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.intensity = 1.0f;
                }

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowCompositePipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowCompositePipeLayout_, 0, 1, &glowCompositeDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowCompositePipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowCompositePush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            case PostPass::XRay: {
                // V1 note: each in-viewport-pass draw independently samples sceneColor
                // and writes the full screen. If multiple effect kinds were active,
                // later passes would overwrite earlier ones. In practice, a single
                // effect kind is active per frame (one selected object, one kind).
                const EffectStyle* xs = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::XRay) {
                        xs = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                XRayPush pc{};
                if (xs) {
                    pc.color[0]  = xs->color.x;
                    pc.color[1]  = xs->color.y;
                    pc.color[2]  = xs->color.z;
                    pc.color[3]  = 1.0f;
                    pc.intensity = xs->intensity;
                } else {
                    // Fallback defaults (shouldn't happen if XRay is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.intensity = 0.5f;
                }

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, xrayPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        xrayPipeLayout_, 0, 1, &xrayDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, xrayPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(XRayPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            // GlowBlurH, GlowBlurV are offscreen passes — handled in runChainOffscreenPasses.
            default:
                break;
        }
    }
}

void VkPostProcess::runChainOffscreenPasses(VkCommandBuffer cb,
                                            const std::vector<PostPass>& passes,
                                            const EffectTable& effects,
                                            VkExtent2D swapExtent) {
    // Find GlowOutline style (used for both blur passes' push constants).
    const EffectStyle* gs = nullptr;
    for (int id = 1; id < EffectTable::kMaxIds; ++id) {
        if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::GlowOutline) {
            gs = &effects.style(static_cast<uint8_t>(id));
            break;
        }
    }

    for (const PostPass pass : passes) {
        switch (pass) {
            case PostPass::GlowBlurH: {
                // Horizontal blur: read mask -> write scratch[0].
                // Runs in glowPass_ / glowFb_[0], outside the swapchain pass.
                GlowBlurPush pc{};
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;
                pc.radius   = gs ? gs->width : 2.0f;
                pc._pad     = 0.0f;

                VkClearValue clearVal{};
                clearVal.color.float32[0] = 0.0f;

                VkRenderPassBeginInfo rpBegin{};
                rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpBegin.renderPass        = glowPass_;
                rpBegin.framebuffer       = glowFb_[0];  // H writes scratch[0]
                rpBegin.renderArea.offset = {0, 0};
                rpBegin.renderArea.extent = extent_;
                rpBegin.clearValueCount   = 1;
                rpBegin.pClearValues      = &clearVal;
                vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

                // Full-screen viewport for the scratch target.
                VkViewport vp{};
                vp.x = 0.0f; vp.y = 0.0f;
                vp.width  = static_cast<float>(extent_.width);
                vp.height = static_cast<float>(extent_.height);
                vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);
                VkRect2D scissor{{0, 0}, extent_};
                vkCmdSetScissor(cb, 0, 1, &scissor);

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowBlurHPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowBlurHPipeLayout_, 0, 1, &glowBlurHDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowBlurHPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowBlurPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                vkCmdEndRenderPass(cb);
                break;
            }

            case PostPass::GlowBlurV: {
                // Vertical blur: read scratch[0] -> write scratch[1].
                GlowBlurPush pc{};
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;
                pc.radius   = gs ? gs->width : 2.0f;
                pc._pad     = 0.0f;

                VkClearValue clearVal{};
                clearVal.color.float32[0] = 0.0f;

                VkRenderPassBeginInfo rpBegin{};
                rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpBegin.renderPass        = glowPass_;
                rpBegin.framebuffer       = glowFb_[1];  // V writes scratch[1]
                rpBegin.renderArea.offset = {0, 0};
                rpBegin.renderArea.extent = extent_;
                rpBegin.clearValueCount   = 1;
                rpBegin.pClearValues      = &clearVal;
                vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

                VkViewport vp{};
                vp.x = 0.0f; vp.y = 0.0f;
                vp.width  = static_cast<float>(extent_.width);
                vp.height = static_cast<float>(extent_.height);
                vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);
                VkRect2D scissor{{0, 0}, extent_};
                vkCmdSetScissor(cb, 0, 1, &scissor);

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowBlurVPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowBlurVPipeLayout_, 0, 1, &glowBlurVDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowBlurVPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowBlurPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                vkCmdEndRenderPass(cb);
                break;
            }

            // All other passes (Copy, Outline, GlowComposite, XRay) run in runChain
            // inside the viewport pass — nothing to do here.
            default:
                break;
        }
    }
}

bool VkPostProcess::createXRayPipeline(VkContext& ctx) {
    // Compile shaders (reuse kFullscreenVert from the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kXrayFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: x-ray shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: x-ray fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: 4 bindings (all COMBINED_IMAGE_SAMPLER, fragment stage).
    //   binding 0: sampler2D  uScene      (scene color, linear sampler_)
    //   binding 1: usampler2D uMask       (mask id, NEAREST maskSampler_)
    //   binding 2: sampler2D  uMaskDepth  (tagged object depth, NEAREST maskSampler_)
    //   binding 3: sampler2D  uSceneDepth (full scene depth, NEAREST maskSampler_)
    // Depth images use NEAREST to avoid filtering across depth edges.
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (int i = 0; i < 4; ++i) {
        bindings[i].binding         = static_cast<uint32_t>(i);
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 4;
    dslInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &xraySetLayout_));

    // Push constant for x-ray params (fragment stage).
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(XRayPush));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &xraySetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &xrayPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = xrayPipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_ (must match its dependencyCount)
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult xrayPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                         &pInfo, nullptr, &xrayPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(xrayPipeResult);

    // Allocate the x-ray descriptor set from the shared pool.
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &xraySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &xrayDescSet_));

    // Write the x-ray descriptor set:
    //   binding 0: scene color   (linear sampler — floating-point color)
    //   binding 1: mask id       (NEAREST maskSampler_ — integer texture)
    //   binding 2: mask depth    (NEAREST maskSampler_ — depth, avoid edge filtering)
    //   binding 3: scene depth   (NEAREST maskSampler_ — depth, avoid edge filtering)
    VkDescriptorImageInfo imgInfos[4]{};
    imgInfos[0].sampler     = sampler_;
    imgInfos[0].imageView   = sceneColorView_;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = maskSampler_;
    imgInfos[1].imageView   = maskColorView_;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = maskSampler_;
    imgInfos[2].imageView   = maskDepthView_;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imgInfos[3].sampler     = maskSampler_;
    imgInfos[3].imageView   = sceneDepthView_;
    imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = xrayDescSet_;
        writes[i].dstBinding      = static_cast<uint32_t>(i);
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 4, writes, 0, nullptr);

    return true;
}

}  // namespace iron
