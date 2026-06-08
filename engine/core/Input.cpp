#include "core/Input.h"

#include <GLFW/glfw3.h>
#include <unordered_map>

namespace {
// One registry entry per window: the owning Input plus the scroll callback that
// was installed before us (so we can chain to it — e.g. ImGui's — and restore it
// when the Input is destroyed). Per-window so multiple windows don't clobber a
// single global previous-callback pointer.
struct ScrollEntry { iron::Input* input = nullptr; GLFWscrollfun prev = nullptr; };
std::unordered_map<GLFWwindow*, ScrollEntry>& inputRegistry() {
    static std::unordered_map<GLFWwindow*, ScrollEntry> r;
    return r;
}
void scrollCallback(GLFWwindow* w, double xoff, double yoff) {
    auto& reg = inputRegistry();
    const auto it = reg.find(w);
    if (it != reg.end()) {
        if (it->second.input) it->second.input->addScroll(yoff);
        if (it->second.prev)  it->second.prev(w, xoff, yoff);   // chain (ImGui etc.)
    }
}
}  // namespace

namespace iron {

Input::Input(GLFWwindow* window) : window_(window) {
    if (window_) {
        glfwGetCursorPos(window_, &mouseX_, &mouseY_);
        prevMouseX_ = mouseX_;
        prevMouseY_ = mouseY_;
        const GLFWscrollfun prev = glfwSetScrollCallback(window_, scrollCallback);
        inputRegistry()[window_] = ScrollEntry{this, prev};
    }
}

Input::~Input() {
    if (!window_) return;
    auto& reg = inputRegistry();
    const auto it = reg.find(window_);
    if (it != reg.end() && it->second.input == this) {
        glfwSetScrollCallback(window_, it->second.prev);   // restore prior callback
        reg.erase(it);
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
