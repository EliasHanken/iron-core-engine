// Iron Core Engine — Visual showcase scene.
// One composed scene that exercises every visual feature shipped to date.
// Inspect with WASD (move), QE (down/up), mouse (look), ESC (quit).
// Task 4: skeleton only — empty world, just the camera + clear.

#include "core/Log.h"
#include "core/Window.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FreeFlyCamera.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <span>

namespace {
constexpr int kScreenWidth  = 1280;
constexpr int kScreenHeight = 720;
}  // namespace

int main() {
    iron::Window window(kScreenWidth, kScreenHeight, "Iron Core — Showcase");
    if (!window.valid()) {
        iron::Log::error("showcase: failed to create window");
        return 1;
    }

    // Capture cursor for mouse-look. Window already loaded glad internally.
    window.setCursorCaptured(true);

    iron::OpenGLRenderer renderer;
    renderer.setViewport(kScreenWidth, kScreenHeight);

    iron::FreeFlyCamera camera;
    camera.position = iron::Vec3{8.0f, 4.0f, 12.0f};
    camera.yaw   = -0.5f;
    camera.pitch = -0.25f;

    double lastMouseX = 0.0, lastMouseY = 0.0;
    glfwGetCursorPos(window.handle(), &lastMouseX, &lastMouseY);

    auto prevTime = std::chrono::steady_clock::now();

    while (!window.shouldClose()) {
        window.pollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;

        if (glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window.handle(), &mouseX, &mouseY);
        const float mouseDx = static_cast<float>(mouseX - lastMouseX);
        const float mouseDy = static_cast<float>(mouseY - lastMouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        const bool kW = glfwGetKey(window.handle(), GLFW_KEY_W) == GLFW_PRESS;
        const bool kS = glfwGetKey(window.handle(), GLFW_KEY_S) == GLFW_PRESS;
        const bool kA = glfwGetKey(window.handle(), GLFW_KEY_A) == GLFW_PRESS;
        const bool kD = glfwGetKey(window.handle(), GLFW_KEY_D) == GLFW_PRESS;
        const bool kQ = glfwGetKey(window.handle(), GLFW_KEY_Q) == GLFW_PRESS;
        const bool kE = glfwGetKey(window.handle(), GLFW_KEY_E) == GLFW_PRESS;

        // FreeFlyCamera::update signature: (dt, mouseDx, mouseDy,
        //   fwd, back, left, right, worldDown, worldUp)
        // Q = worldDown, E = worldUp.
        camera.update(dt, mouseDx, mouseDy, kW, kS, kA, kD, kQ, kE);

        const iron::Mat4 projection = iron::perspective(
            camera.fovDeg * 3.14159265f / 180.0f,
            static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight),
            0.1f, 200.0f);

        const iron::DirectionalLight sun{};
        const iron::Fog fog{};

        renderer.beginFrame(iron::Vec3{0.5f, 0.6f, 0.8f},
                            sun,
                            std::span<const iron::PointLight>{},
                            fog,
                            camera.viewMatrix(),
                            projection);
        renderer.endFrame();

        window.swapBuffers();
    }
    return 0;
}
