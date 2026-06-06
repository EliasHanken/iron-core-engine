#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class Window;
class Renderer;

// Integrates Dear ImGui with the engine's Vulkan renderer and GLFW window.
// The editor module is Vulkan-only; init() expects the renderer to be the
// VulkanRenderer (always true in a Vulkan build). ImGui records into the
// swapchain (final) render pass via the renderer's deferred-UI hook, AFTER the
// post-process composite, so editor chrome is never affected by scene effects.
//
// Usage per frame:
//   layer.beginFrame();       // before building any ImGui windows
//   ...build ImGui windows... // (panels)
//   layer.render();           // after windows; enqueues the UI as an overlay
//   renderer.endFrame();      // draws the enqueued UI on top of the scene
class ImGuiLayer {
public:
    bool init(Window& window, Renderer& renderer);
    void beginFrame();
    void render();
    void shutdown();

    // True while ImGui owns the pointer / keyboard (hovering or editing a
    // widget). The host suppresses camera/game input when these are true.
    bool wantsMouse() const;
    bool wantsKeyboard() const;

    // Fullscreen dockspace host. Call right after beginFrame(); draw panels
    // (including the Viewport) between beginDockspace() and endDockspace() so
    // they dock into the central space. endDockspace() closes the host window.
    void beginDockspace();
    void endDockspace();

    // Bind the offscreen viewport image as an ImGui texture id (for ImGui::Image
    // in the Viewport panel). Cached: rebinds (frees the old via RemoveTexture)
    // only when (view, sampler) change — e.g. on viewport resize. Returns the
    // ImTextureID as void* (header stays ImGui-type-free); caller casts to
    // ImTextureID. Returns nullptr if not initialized or handles are null.
    void* viewportTexture(VkImageView view, VkSampler sampler);

    // Upload a small RGBA8 texture and return an ImGui texture id (void* /
    // VkDescriptorSet), valid for the layer's lifetime. Returns nullptr if not
    // initialized. Used for the node header gradient.
    void* registerTexture(const unsigned char* rgba, int width, int height);

private:
    bool initialized_ = false;
    void* device_ = nullptr;          // VkDevice, stored opaquely for shutdown
    void* descriptorPool_ = nullptr;  // VkDescriptorPool, owned by this layer
    Renderer* renderer_ = nullptr;

    void*       viewportTexId_      = nullptr;            // ImTextureID (VkDescriptorSet), cast to void*
    VkImageView viewportTexView_    = VK_NULL_HANDLE;     // last-bound view (change-detect)
    VkSampler   viewportTexSampler_ = VK_NULL_HANDLE;     // last-bound sampler

    // ImGui descriptor sets created by registerTexture(); freed in shutdown().
    // The backing VkImage/view/sampler are owned by the renderer's texture
    // store, so only the ImGui binding is ours to remove here.
    std::vector<void*> registeredTextures_;
};

}  // namespace iron
