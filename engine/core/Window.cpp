#include "core/Window.h"

#include "core/Log.h"

#ifdef IRON_RENDER_BACKEND_OPENGL
#include <glad/gl.h>
#endif
#include <GLFW/glfw3.h>

namespace iron {

namespace {
// GLFW is initialized at most once per process; glfwTerminate() is intentionally
// omitted because the engine owns GLFW for the process lifetime.
bool g_glfwInitialized = false;

void framebufferSizeCallback(GLFWwindow*, int w, int h) {
#ifdef IRON_RENDER_BACKEND_OPENGL
    glViewport(0, 0, w, h);
#else
    (void)w; (void)h;  // Vulkan: swapchain recreate is driven by Renderer::setViewport
#endif
}
} // namespace

Window::Window(int width, int height, const std::string& title)
    : width_(width), height_(height) {
    if (!g_glfwInitialized) {
        if (!glfwInit()) {
            Log::error("Window: glfwInit failed");
            return;
        }
        g_glfwInitialized = true;
    }

#ifdef IRON_RENDER_BACKEND_VULKAN
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    handle_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!handle_) {
        Log::error("Window: glfwCreateWindow failed");
        return;
    }

#ifdef IRON_RENDER_BACKEND_OPENGL
    glfwMakeContextCurrent(handle_);
    if (gladLoadGL(glfwGetProcAddress) == 0) {
        Log::error("Window: failed to load OpenGL functions");
        glfwDestroyWindow(handle_);
        handle_ = nullptr;
        return;
    }
    glViewport(0, 0, width, height);
#endif

    glfwSetFramebufferSizeCallback(handle_, framebufferSizeCallback);

    glfwFocusWindow(handle_);

#ifdef IRON_RENDER_BACKEND_OPENGL
    Log::info("Window: OpenGL %s",
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
#else
    Log::info("Window: Vulkan-mode window created (no GL context)");
#endif
}

Window::~Window() {
    if (handle_) {
        glfwDestroyWindow(handle_);
    }
}

bool Window::shouldClose() const {
    return handle_ == nullptr || glfwWindowShouldClose(handle_);
}

void Window::pollEvents() {
    if (handle_) {
        glfwPollEvents();
    }
}

void Window::swapBuffers() {
#ifdef IRON_RENDER_BACKEND_OPENGL
    if (handle_) {
        glfwSwapBuffers(handle_);
    }
#endif
    // Vulkan: swap happens via vkQueuePresentKHR inside Renderer::endFrame.
}

void Window::setCursorCaptured(bool captured) {
    if (!handle_) {
        return;
    }
    glfwSetInputMode(handle_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace iron
