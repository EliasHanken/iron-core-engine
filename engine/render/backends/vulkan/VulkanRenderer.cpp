// VulkanRenderer.cpp — top-level orchestrator for the Vulkan backend.
// In M9 most methods are stubs; foundation tasks 4-11 fill in init
// and the per-frame pipeline.

#include "render/backends/vulkan/VulkanRenderer.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"
#include "core/Window.h"

#include <vulkan/vulkan.h>

namespace iron {

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    if (initOk_) {
        // Wait for outstanding GPU work before tearing down.
        vkDeviceWaitIdle(context_.device());
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
    initOk_ = true;
    Log::info("VulkanRenderer: context + swapchain up (foundation; features still stubs)");
    return true;
}

void VulkanRenderer::warnOnce(const char* feature) {
    if (warnedFeatures_.insert(feature).second) {
        Log::warn("Vulkan: %s not implemented yet (stub)", feature);
    }
}

// --- resource creation stubs (filled in Tasks 8-9) ---

MeshHandle VulkanRenderer::createMesh(const MeshData&) {
    warnOnce("createMesh");
    return kInvalidHandle;
}
void VulkanRenderer::updateMesh(MeshHandle, const MeshData&) {
    warnOnce("updateMesh");
}
TextureHandle VulkanRenderer::createTexture(int, int, const unsigned char*) {
    warnOnce("createTexture");
    return kInvalidHandle;
}
TextureHandle VulkanRenderer::loadTexture(const std::string&) {
    warnOnce("loadTexture");
    return kInvalidHandle;
}
TextureHandle VulkanRenderer::whiteTexture() const { return kInvalidHandle; }
TextureHandle VulkanRenderer::flatNormalTexture() const { return kInvalidHandle; }
TextureHandle VulkanRenderer::noSpecularTexture() const { return kInvalidHandle; }
ShaderHandle VulkanRenderer::createShader(const std::string&, const std::string&) {
    warnOnce("createShader");
    return kInvalidHandle;
}

// --- M9 stubs (M10+ work) ---

CubemapHandle VulkanRenderer::createCubemap(int, int,
    const std::array<const unsigned char*, 6>&) {
    warnOnce("createCubemap");
    return kInvalidHandle;
}
void VulkanRenderer::setSkybox(CubemapHandle) { warnOnce("setSkybox"); }
void VulkanRenderer::setShadowBounds(Vec3, float) { warnOnce("setShadowBounds"); }
void VulkanRenderer::setReflectionPlane(Vec3, float) { warnOnce("setReflectionPlane"); }
void VulkanRenderer::disableReflectionPlane() { warnOnce("disableReflectionPlane"); }
void VulkanRenderer::drawLine(Vec3, Vec3, Vec3) { warnOnce("drawLine"); }
void VulkanRenderer::flushDebugLines(const Mat4&, const Mat4&) { warnOnce("flushDebugLines"); }
void VulkanRenderer::drawHud(const HudBatch&, int, int) { warnOnce("drawHud"); }

// --- per-frame (filled in Task 11) ---

void VulkanRenderer::beginFrame(Vec3, const DirectionalLight&,
                                std::span<const PointLight>,
                                const Fog&, const Mat4&, const Mat4&) {
    warnOnce("beginFrame");
}
void VulkanRenderer::submit(const DrawCall&) { warnOnce("submit"); }
void VulkanRenderer::endFrame() { warnOnce("endFrame"); }
void VulkanRenderer::setViewport(int, int) { warnOnce("setViewport"); }

}  // namespace iron
