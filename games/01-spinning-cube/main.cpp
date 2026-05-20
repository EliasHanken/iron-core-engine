#include <GLFW/glfw3.h>
#include <cstdio>

int main() {
    if (!glfwInit()) {
        std::printf("Failed to init GLFW\n");
        return 1;
    }
    std::printf("Iron Core Engine - GLFW %s\n", glfwGetVersionString());
    glfwTerminate();
    return 0;
}
