#include "render/backends/opengl/GLDebugLines.h"

#include <glad/gl.h>

#include <cstddef>

namespace iron {

namespace {
const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform mat4 uViewProjection;

out vec3 vColor;

void main() {
    vColor = aColor;
    gl_Position = uViewProjection * vec4(aPos, 1.0);
}
)";

const char* kFragmentSrc = R"(#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 1.0);
}
)";
}  // namespace

GLDebugLines::GLDebugLines() : shader_(kVertexSrc, kFragmentSrc) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, color)));

    glBindVertexArray(0);
}

GLDebugLines::~GLDebugLines() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLDebugLines::addLine(Vec3 a, Vec3 b, Vec3 color) {
    vertices_.push_back(Vertex{a, color});
    vertices_.push_back(Vertex{b, color});
}

void GLDebugLines::flush(const Mat4& view, const Mat4& projection) {
    if (vertices_.empty()) {
        return;
    }
    // If the debug shader failed to build there is nothing to draw with —
    // still clear the queue so it does not grow without bound.
    if (!shader_.isValid()) {
        vertices_.clear();
        return;
    }

    shader_.bind();
    shader_.setMat4("uViewProjection", projection * view);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)),
                 vertices_.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_.size()));
    glBindVertexArray(0);

    vertices_.clear();
}

} // namespace iron
