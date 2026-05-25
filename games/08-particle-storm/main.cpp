// games/08-particle-storm/main.cpp — Vulkan-only GPU compute particle showcase.
// Free-fly through 1 M particles riding a curl-noise field.
// Controls: WASD (move), Left Ctrl / Space (down / up), mouse (look), ESC (quit).

#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "math/Transform.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/ParticleSystem.h"
#include "scene/FreeFlyCamera.h"

#include <GLFW/glfw3.h>

#include <numbers>
#include <span>

int main() {
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - Particle Storm";
    cfg.width  = 1280;
    cfg.height = 720;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("particle-storm: Application init failed");
        return 1;
    }

    auto renderer = iron::createRenderer(app.window());
    if (!renderer) {
        iron::Log::error("particle-storm: renderer init failed");
        return 1;
    }

    iron::ParticleSystemConfig psc;  // defaults: 1M particles, curl-noise field
    auto particles = iron::createParticleSystem(*renderer, psc);
    if (!particles) {
        iron::Log::error("particle-storm: particle system init failed");
        return 1;
    }

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 0.0f, 45.0f};

    // Projection matrix — built manually so we control FoV and clip planes.
    const float aspect = static_cast<float>(cfg.width) / static_cast<float>(cfg.height);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 500.0f);

    app.window().setCursorCaptured(true);

    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();

        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        // Input already tracks deltas internally — no manual glfwGetCursorPos needed.
        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());

        cam.update(t.deltaSeconds, mdx, mdy,
                   input.keyDown(GLFW_KEY_W),
                   input.keyDown(GLFW_KEY_S),
                   input.keyDown(GLFW_KEY_A),
                   input.keyDown(GLFW_KEY_D),
                   input.keyDown(GLFW_KEY_LEFT_CONTROL),
                   input.keyDown(GLFW_KEY_SPACE),
                   /*moveSpeed*/ 12.0f);

        particles->tick(t.deltaSeconds);
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();

        // Minimal scene: no sun, no point lights, no fog — just the particle cloud.
        const iron::DirectionalLight noSun{};
        const iron::Fog noFog{};

        renderer->beginFrame(
            iron::Vec3{0.02f, 0.02f, 0.06f},   // deep-blue clear colour
            noSun,
            std::span<const iron::PointLight>{},
            noFog,
            view, proj);

        particles->render(view, proj);

        renderer->endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
}
