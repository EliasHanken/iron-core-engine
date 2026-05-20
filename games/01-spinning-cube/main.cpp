#include "core/Log.h"

#include <GLFW/glfw3.h>

int main() {
    if (!glfwInit()) {
        iron::Log::error("Failed to init GLFW");
        return 1;
    }
    iron::Log::info("Iron Core Engine - GLFW %s", glfwGetVersionString());
    glfwTerminate();
    return 0;
}
