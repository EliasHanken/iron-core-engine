#include "core/Application.h"
#include "core/Log.h"

#include <GLFW/glfw3.h>

namespace iron {

Application::Application(const Config& config)
    : window_(config.width, config.height, config.title),
      fixedStep_(static_cast<double>(config.fixedStep)) {}

void Application::run() {
    if (!window_.valid()) {
        Log::error("Application::run called on an invalid window");
        return;
    }

    double previous = glfwGetTime();
    double accumulator = 0.0;
    double simTime = 0.0;

    while (!window_.shouldClose()) {
        const double now = glfwGetTime();
        accumulator += now - previous;
        previous = now;

        // Avoid a spiral of death if the app was paused / stalled.
        if (accumulator > 0.25) {
            accumulator = 0.25;
        }

        window_.pollEvents();

        while (accumulator >= fixedStep_) {
            if (update_) {
                FrameTime t;
                t.deltaSeconds = static_cast<float>(fixedStep_);
                t.totalSeconds = static_cast<float>(simTime);
                update_(t);
            }
            accumulator -= fixedStep_;
            simTime += fixedStep_;
        }

        if (render_) {
            render_();
        }
        window_.swapBuffers();
    }
}

} // namespace iron
