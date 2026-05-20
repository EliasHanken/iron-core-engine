#pragma once

#include <string>

struct GLFWwindow;

namespace iron {

// Owns a GLFW window plus its OpenGL 3.3 core context. Loads GL function
// pointers via glad on construction. RAII: the window is destroyed with the
// object.
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();
    void swapBuffers();

    int width() const { return width_; }
    int height() const { return height_; }
    GLFWwindow* handle() const { return handle_; }

    bool valid() const { return handle_ != nullptr; }

private:
    GLFWwindow* handle_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace iron
