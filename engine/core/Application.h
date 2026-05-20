#pragma once

#include "core/Time.h"
#include "core/Window.h"

#include <functional>
#include <string>

namespace iron {

// Owns the window and runs a fixed-timestep game loop.
//
// The simulation advances in fixed steps (default 60 Hz) so physics and game
// logic are deterministic regardless of frame rate. Rendering happens once per
// real frame, as fast as the machine allows.
class Application {
public:
    struct Config {
        int width = 960;
        int height = 540;
        std::string title = "Iron Core Engine";
        float fixedStep = 1.0f / 60.0f;
    };

    explicit Application(const Config& config);

    // Called zero or more times per frame with a fixed delta.
    void setUpdate(std::function<void(const FrameTime&)> fn) { update_ = std::move(fn); }
    // Called exactly once per frame, after updates.
    void setRender(std::function<void()> fn) { render_ = std::move(fn); }

    void run();

    Window& window() { return window_; }
    bool valid() const { return window_.valid(); }

private:
    Window window_;
    double fixedStep_;
    std::function<void(const FrameTime&)> update_;
    std::function<void()> render_;
};

} // namespace iron
