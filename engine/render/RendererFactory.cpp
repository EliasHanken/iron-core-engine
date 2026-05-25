#include "render/RendererFactory.h"

#include "core/Log.h"

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VulkanRenderer.h"
#else
#include "render/backends/opengl/OpenGLRenderer.h"
#endif

namespace iron {

std::unique_ptr<Renderer> createRenderer(Window& window) {
#ifdef IRON_RENDER_BACKEND_VULKAN
    auto r = std::make_unique<VulkanRenderer>();
    if (!r->init(window)) {
        Log::error("createRenderer: VulkanRenderer init failed");
    }
    return r;
#else
    (void)window;  // OpenGLRenderer reads the current context, not the Window.
    return std::make_unique<OpenGLRenderer>();
#endif
}

}  // namespace iron
