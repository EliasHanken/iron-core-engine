#include "render/backends/opengl/GLHud.h"

#include <glad/gl.h>

#include <cstddef>

namespace iron {

namespace {
const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform vec2 uScreenSize;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    // Pixel space (origin top-left, y down) -> NDC (origin centre, y up).
    float ndcX = aPos.x / uScreenSize.x * 2.0 - 1.0;
    float ndcY = 1.0 - aPos.y / uScreenSize.y * 2.0;
    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);
}
)";

const char* kFragmentSrc = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    FragColor = texture(uTexture, vUV) * vColor;
}
)";
}  // namespace

GLHud::GLHud() : shader_(kVertexSrc, kFragmentSrc) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(HudVertex),
                          reinterpret_cast<void*>(offsetof(HudVertex, color)));

    glBindVertexArray(0);
}

GLHud::~GLHud() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLHud::begin(int framebufferWidth, int framebufferHeight) {
    if (!shader_.isValid()) {
        return;
    }
    shader_.bind();
    shader_.setVec2("uScreenSize",
                    Vec2{static_cast<float>(framebufferWidth),
                         static_cast<float>(framebufferHeight)});
    shader_.setInt("uTexture", 0);
}

void GLHud::drawGroup(const std::vector<HudVertex>& vertices) {
    if (vertices.empty() || !shader_.isValid()) {
        return;
    }
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(HudVertex)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glBindVertexArray(0);
}

void GLHud::end() {
    // Nothing to release per-frame; state is restored by the caller.
}

} // namespace iron
