#include "core/Window.h"

#include "core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace iron {

namespace {
// GLFW is initialized at most once per process; glfwTerminate() is intentionally
// omitted because the engine owns GLFW for the process lifetime.
bool g_glfwInitialized = false;

void framebufferSizeCallback(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
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

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    handle_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!handle_) {
        Log::error("Window: glfwCreateWindow failed");
        return;
    }

    glfwMakeContextCurrent(handle_);
    if (gladLoadGL(glfwGetProcAddress) == 0) {
        Log::error("Window: failed to load OpenGL functions");
        glfwDestroyWindow(handle_);
        handle_ = nullptr;
        return;
    }

    glfwSetFramebufferSizeCallback(handle_, framebufferSizeCallback);
    glViewport(0, 0, width, height);

    // Explicitly claim input focus. GLFW requests it on creation, but the OS
    // can deny focus-stealing when the launching process isn't in front (e.g.
    // started from a terminal) — leaving the window visible but deaf to the
    // keyboard until the user clicks it. This makes the request explicit.
    glfwFocusWindow(handle_);

    Log::info("Window: OpenGL %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
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
    if (handle_) {
        glfwSwapBuffers(handle_);
    }
}

} // namespace iron
