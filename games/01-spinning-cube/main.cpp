#include "core/Application.h"
#include "core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Spinning Cube";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    app.setUpdate([&app](const iron::FrameTime&) {
        if (app.input().keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
    });

    app.setRender([] {
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    });

    app.run();
    return 0;
}
