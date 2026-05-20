#include "core/Log.h"
#include "core/Window.h"

#include <glad/gl.h>

int main() {
    iron::Window window(960, 540, "Iron Core Engine - Spinning Cube");
    if (!window.valid()) {
        iron::Log::error("Window creation failed");
        return 1;
    }

    while (!window.shouldClose()) {
        window.pollEvents();
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        window.swapBuffers();
    }
    return 0;
}
