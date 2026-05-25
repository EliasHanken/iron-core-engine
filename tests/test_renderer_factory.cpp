// Verifies that createRenderer() returns a non-null Renderer of the
// expected concrete type for the current build's backend. Uses a
// hidden GLFW window to stay headless.

#include "test_framework.h"
#include "core/Window.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VulkanRenderer.h"
#endif
#ifdef IRON_RENDER_BACKEND_OPENGL
#include "render/backends/opengl/OpenGLRenderer.h"
#endif

#include <GLFW/glfw3.h>

using namespace iron;

int main() {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    Window window(64, 64, "test_renderer_factory");

    if (!window.valid()) {
        // Headless CI runner with no display — glfwCreateWindow returned null.
        // Can't construct a Renderer against an invalid window. Skip.
        std::printf("OK - skipped (no display / glfwCreateWindow failed)\n");
        return 0;
    }

    auto r = createRenderer(window);
    CHECK(r != nullptr);

#ifdef IRON_RENDER_BACKEND_VULKAN
    auto* concrete = dynamic_cast<VulkanRenderer*>(r.get());
    if (!concrete) {
        std::printf("VulkanRenderer cast failed\n");
        return 1;
    }
    if (!concrete->initOk()) {
        // No working ICD on this machine — skip instead of fail.
        std::printf("OK - skipped (no Vulkan ICD)\n");
        return 0;
    }
#endif

#ifdef IRON_RENDER_BACKEND_OPENGL
    CHECK(dynamic_cast<OpenGLRenderer*>(r.get()) != nullptr);
#endif

    return iron_test_result();
}
