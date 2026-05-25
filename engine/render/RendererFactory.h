#pragma once

#include "render/Renderer.h"

#include <memory>

namespace iron {

class Window;

// Returns a concrete Renderer selected at build time by the
// IRON_RENDER_BACKEND_OPENGL / IRON_RENDER_BACKEND_VULKAN define.
// Caller owns the returned pointer; renderer's lifetime ends when
// the unique_ptr is destroyed.
//
// If the chosen backend's init fails (no compatible GPU, missing
// driver, etc.) the renderer is still returned, but its methods will
// fail safely — game code should check renderer->initOk() before
// using it.
std::unique_ptr<Renderer> createRenderer(Window& window);

}  // namespace iron
