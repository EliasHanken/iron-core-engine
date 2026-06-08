#include "core/Input.h"

#include <GLFW/glfw3.h>
#include <unordered_map>

namespace {
std::unordered_map<GLFWwindow*, iron::Input*>& inputRegistry() {
    static std::unordered_map<GLFWwindow*, iron::Input*> r;
    return r;
}
GLFWscrollfun g_prevScroll = nullptr;
void scrollCallback(GLFWwindow* w, double xoff, double yoff) {
    auto& reg = inputRegistry();
    const auto it = reg.find(w);
    if (it != reg.end() && it->second) it->second->addScroll(yoff);
    if (g_prevScroll) g_prevScroll(w, xoff, yoff);   // chain (ImGui etc.)
}
}  // namespace

namespace iron {

Input::Input(GLFWwindow* window) : window_(window) {
    if (window_) {
        glfwGetCursorPos(window_, &mouseX_, &mouseY_);
        prevMouseX_ = mouseX_;
        prevMouseY_ = mouseY_;
        inputRegistry()[window_] = this;
        g_prevScroll = glfwSetScrollCallback(window_, scrollCallback);
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
    scrollThisFrame_ = scrollAccum_;
    scrollAccum_ = 0.0;
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
    // Reads the per-step snapshot taken in update(), so it stays consistent
    // with keyDown / mouseButtonPressed rather than polling GLFW live.
    return button >= 0 && button < kMouseButtonCount && currentMouse_[button];
}

bool Input::mouseButtonPressed(int button) const {
    return button >= 0 && button < kMouseButtonCount
           && currentMouse_[button] && !previousMouse_[button];
}

bool Input::mouseButtonReleased(int button) const {
    return button >= 0 && button < kMouseButtonCount
           && !currentMouse_[button] && previousMouse_[button];
}

void Input::addScroll(double yoff) { scrollAccum_ += yoff; }

} // namespace iron
