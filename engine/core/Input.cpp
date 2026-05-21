#include "core/Input.h"

#include <GLFW/glfw3.h>

namespace iron {

Input::Input(GLFWwindow* window) : window_(window) {
    if (window_) {
        glfwGetCursorPos(window_, &mouseX_, &mouseY_);
        prevMouseX_ = mouseX_;
        prevMouseY_ = mouseY_;
    }
}

void Input::update() {
    static_assert(Input::kKeyCount > GLFW_KEY_LAST,
                  "Input::kKeyCount must exceed GLFW_KEY_LAST");
    if (!window_) {
        return;
    }
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
        previous_[key] = current_[key];
        current_[key] = glfwGetKey(window_, key) == GLFW_PRESS;
    }
    prevMouseX_ = mouseX_;
    prevMouseY_ = mouseY_;
    glfwGetCursorPos(window_, &mouseX_, &mouseY_);
    for (int button = 0; button < kMouseButtonCount; ++button) {
        previousMouse_[button] = currentMouse_[button];
        currentMouse_[button] =
            glfwGetMouseButton(window_, button) == GLFW_PRESS;
    }
}

bool Input::keyDown(int key) const {
    return key >= 0 && key < kKeyCount && current_[key];
}

bool Input::keyPressed(int key) const {
    return key >= 0 && key < kKeyCount && current_[key] && !previous_[key];
}

bool Input::keyReleased(int key) const {
    return key >= 0 && key < kKeyCount && !current_[key] && previous_[key];
}

bool Input::mouseButtonDown(int button) const {
    return window_ && glfwGetMouseButton(window_, button) == GLFW_PRESS;
}

bool Input::mouseButtonPressed(int button) const {
    return button >= 0 && button < kMouseButtonCount
           && currentMouse_[button] && !previousMouse_[button];
}

} // namespace iron
