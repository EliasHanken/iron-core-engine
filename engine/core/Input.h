#pragma once

struct GLFWwindow;

namespace iron {

// Polls keyboard and mouse state for one window. Call update() once per frame
// (the Application does this); query with the accessors afterwards.
//
// Key codes are GLFW key codes (GLFW_KEY_W, etc.) so callers include GLFW.
class Input {
public:
    explicit Input(GLFWwindow* window);

    void update();

    bool keyDown(int key) const;            // held this frame
    bool keyPressed(int key) const;         // went down this frame
    bool keyReleased(int key) const;        // went up this frame

    double mouseX() const { return mouseX_; }
    double mouseY() const { return mouseY_; }
    double mouseDeltaX() const { return mouseX_ - prevMouseX_; }
    double mouseDeltaY() const { return mouseY_ - prevMouseY_; }
    bool mouseButtonDown(int button) const;
    bool mouseButtonPressed(int button) const;  // went down this frame
    bool mouseButtonReleased(int button) const; // went up this frame
    double scrollDelta() const { return scrollThisFrame_; }  // wheel since last update()

    void addScroll(double yoff);  // called by the file-local GLFW scroll callback

private:
    static constexpr int kKeyCount = 350;   // GLFW_KEY_LAST is 348
    static constexpr int kMouseButtonCount = 8;  // GLFW_MOUSE_BUTTON_LAST is 7

    GLFWwindow* window_;
    bool current_[kKeyCount] = {};
    bool previous_[kKeyCount] = {};
    bool currentMouse_[kMouseButtonCount] = {};
    bool previousMouse_[kMouseButtonCount] = {};
    double mouseX_ = 0.0, mouseY_ = 0.0;
    double prevMouseX_ = 0.0, prevMouseY_ = 0.0;
    double scrollAccum_ = 0.0;      // accumulated by the GLFW scroll callback
    double scrollThisFrame_ = 0.0;  // snapshot taken in update()
};

} // namespace iron
