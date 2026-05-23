#include "render/backends/opengl/GLSkybox.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

namespace {

// 36-vertex unit cube (6 faces * 2 triangles * 3 verts). Vertex
// positions are used as world-space directions in the skybox shader,
// so the cube must span ±1.
const float kCubeVertices[] = {
    // +X
     1, -1, -1,  1,  1, -1,  1,  1,  1,
     1, -1, -1,  1,  1,  1,  1, -1,  1,
    // -X
    -1, -1,  1, -1,  1,  1, -1,  1, -1,
    -1, -1,  1, -1,  1, -1, -1, -1, -1,
    // +Y
    -1,  1, -1, -1,  1,  1,  1,  1,  1,
    -1,  1, -1,  1,  1,  1,  1,  1, -1,
    // -Y
    -1, -1,  1, -1, -1, -1,  1, -1, -1,
    -1, -1,  1,  1, -1, -1,  1, -1,  1,
    // +Z
    -1, -1,  1,  1, -1,  1,  1,  1,  1,
    -1, -1,  1,  1,  1,  1, -1,  1,  1,
    // -Z
     1, -1, -1, -1, -1, -1, -1,  1, -1,
     1, -1, -1, -1,  1, -1,  1,  1, -1,
};

const char* kSkyboxVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vWorldDir;
void main() {
    vWorldDir = aPos;
    // Strip translation from view so the sky doesn't move with the camera.
    mat4 viewNoTranslation = mat4(mat3(uView));
    vec4 clip = uProjection * viewNoTranslation * vec4(aPos, 1.0);
    // Force gl_FragDepth = 1.0 (far plane) so geometry draws on top.
    gl_Position = clip.xyww;
}
)";

const char* kSkyboxFragmentShader = R"(#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;
uniform samplerCube uSkyCubemap;
uniform vec3 uFogColor;
uniform float uHorizonFogBand;
void main() {
    vec3 dir = normalize(vWorldDir);
    vec3 skyColor = texture(uSkyCubemap, dir).rgb;
    // Blend with fog colour near the horizon. abs(dir.y) is 0 at the
    // horizon and 1 at zenith/nadir. smoothstep ramps smoothly.
    float horizonMix = smoothstep(0.0, uHorizonFogBand, abs(dir.y));
    vec3 result = mix(uFogColor, skyColor, horizonMix);
    FragColor = vec4(result, 1.0);
}
)";

} // namespace

GLSkybox::GLSkybox()
    : shader_(kSkyboxVertexShader, kSkyboxFragmentShader) {
    if (!shader_.isValid()) {
        Log::error("GLSkybox: shader failed to compile");
        return;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

GLSkybox::~GLSkybox() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLSkybox::draw(const Mat4& view, const Mat4& projection,
                    const GLCubemap& sky, Vec3 fogColor,
                    float horizonBand) const {
    if (!isValid() || !sky.isValid()) return;

    // Save state we modify.
    GLboolean savedDepthMask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedDepthFunc;
    glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    shader_.bind();
    shader_.setMat4("uView", view);
    shader_.setMat4("uProjection", projection);
    shader_.setInt("uSkyCubemap", 0);
    shader_.setVec3("uFogColor", fogColor);
    shader_.setFloat("uHorizonFogBand", horizonBand);

    sky.bind(0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    // Restore state.
    glDepthFunc(savedDepthFunc);
    glDepthMask(savedDepthMask);
}

} // namespace iron
