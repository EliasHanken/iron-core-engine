#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>
#include <vector>

namespace iron {

// Accumulates coloured 3D line segments and draws them in one batch with
// GL_LINES. The vertex buffer is re-uploaded on every flush because the line
// set is transient (rebuilt each frame). Requires a current GL context.
class GLDebugLines {
public:
    GLDebugLines();
    ~GLDebugLines();

    GLDebugLines(const GLDebugLines&) = delete;
    GLDebugLines& operator=(const GLDebugLines&) = delete;

    // Queue one line segment for the current frame.
    void addLine(Vec3 a, Vec3 b, Vec3 color);

    // Upload the queued vertices, draw them, and clear the queue.
    void flush(const Mat4& view, const Mat4& projection);

private:
    struct Vertex {
        Vec3 position;
        Vec3 color;
    };

    std::vector<Vertex> vertices_;
    GLShader shader_;
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
};

} // namespace iron
