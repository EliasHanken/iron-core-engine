#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/backends/opengl/GLCubemap.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>

namespace iron {

// Renders a cubemap skybox. Owns the unit cube VBO/VAO and the skybox
// shader. The cube is drawn at the camera position (the view matrix's
// translation is stripped), forced to gl_FragDepth = 1.0 so all
// geometry draws on top.
class GLSkybox {
public:
    GLSkybox();
    ~GLSkybox();

    GLSkybox(const GLSkybox&) = delete;
    GLSkybox& operator=(const GLSkybox&) = delete;

    bool isValid() const { return vao_ != 0 && shader_.isValid(); }

    // Draws the skybox. Caller must have a depth buffer cleared to 1.0
    // (or geometry already rendered such that empty pixels are at
    // depth 1.0). State is saved and restored.
    void draw(const Mat4& view, const Mat4& projection,
              const GLCubemap& sky, Vec3 fogColor, float horizonBand) const;

private:
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    GLShader shader_;
};

} // namespace iron
