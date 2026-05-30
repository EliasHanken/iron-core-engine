#pragma once

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

private:
    bool initialized_ = false;
    void* device_ = nullptr;          // VkDevice, stored opaquely for shutdown
    void* descriptorPool_ = nullptr;  // VkDescriptorPool, owned by this layer
    Renderer* renderer_ = nullptr;
};

}  // namespace iron
