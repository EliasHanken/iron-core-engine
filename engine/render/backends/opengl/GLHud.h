#pragma once

#include "render/HudBatch.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>
#include <vector>

namespace iron {

// The OpenGL screen-space HUD pass. Owns a 2D shader and a dynamic vertex
// buffer. Usage per frame: begin(), then drawGroup() once per HudDrawGroup
// (the caller binds the group's texture to unit 0 first), then end().
// Mirrors GLDebugLines. Requires a current GL context.
class GLHud {
public:
    GLHud();
    ~GLHud();

    GLHud(const GLHud&) = delete;
    GLHud& operator=(const GLHud&) = delete;

    // Binds the shader and sets the framebuffer size for the pixel->NDC map.
    void begin(int framebufferWidth, int framebufferHeight);

    // Uploads and draws one group's vertices as triangles. The caller must
    // bind the group's texture to unit 0 beforehand.
    void drawGroup(const std::vector<HudVertex>& vertices);

    void end();

private:
    GLShader shader_;
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
};

} // namespace iron
