#include "core/Application.h"

#include <GLFW/glfw3.h>

namespace iron {

Application::Application(const Config& config)
    : window_(config.width, config.height, config.title),
      fixedStep_(config.fixedStep) {}

void Application::run() {
    if (!window_.valid()) {
        return;
    }

    const double start = glfwGetTime();
    double previous = start;
    double accumulator = 0.0;

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
                t.deltaSeconds = fixedStep_;
                t.totalSeconds = static_cast<float>(now - start);
                update_(t);
            }
            accumulator -= fixedStep_;
        }

        if (render_) {
            render_();
        }
        window_.swapBuffers();
    }
}

} // namespace iron
