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
} // namespace

// File-scope (in `iron` namespace) so the `friend` declaration in Window.h can
// reach it. Updates Window state on framebuffer resize for both backends; the
// OpenGL viewport call stays inside its existing guard.
void framebufferSizeCallback(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self) return;
    self->width_   = width;
    self->height_  = height;
    self->resized_ = true;
#ifdef IRON_RENDER_BACKEND_OPENGL
    glViewport(0, 0, width, height);
#endif
}

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

#ifdef IRON_RENDER_BACKEND_VULKAN
    const std::string fullTitle = title + " [Vulkan]";
#else
    const std::string fullTitle = title + " [OpenGL]";
#endif
    handle_ = glfwCreateWindow(width, height, fullTitle.c_str(), nullptr, nullptr);
    if (!handle_) {
        Log::error("Window: glfwCreateWindow failed");
        return;
    }
    glfwSetWindowUserPointer(handle_, this);

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

bool Window::consumeResized() {
    if (!resized_) return false;
    resized_ = false;
    return true;
}

} // namespace iron
