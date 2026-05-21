#include "render/backends/opengl/OpenGLRenderer.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

OpenGLRenderer::OpenGLRenderer() {
    glEnable(GL_DEPTH_TEST);

    // A 2x2 magenta/black checker used when a real texture fails to load.
    const unsigned char fallback[16] = {
        255, 0, 255, 255,   0, 0, 0, 255,
        0, 0, 0, 255,       255, 0, 255, 255,
    };
    fallbackTexture_ = createTexture(2, 2, fallback);
}

MeshHandle OpenGLRenderer::createMesh(const MeshData& data) {
    meshes_.push_back(std::make_unique<GLMesh>(data));
    return static_cast<MeshHandle>(meshes_.size());  // index + 1
}

void OpenGLRenderer::updateMesh(MeshHandle mesh, const MeshData& data) {
    // Handles are (index + 1); reject anything out of range.
    if (mesh == kInvalidHandle || mesh > meshes_.size()) {
        Log::warn("OpenGLRenderer::updateMesh: mesh handle out of range");
        return;
    }
    meshes_[mesh - 1]->update(data);
}

TextureHandle OpenGLRenderer::createTexture(int width, int height,
                                            const unsigned char* rgba) {
    auto tex = std::make_unique<GLTexture>(width, height, rgba);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: createTexture produced an invalid texture");
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

TextureHandle OpenGLRenderer::loadTexture(const std::string& path) {
    auto tex = std::make_unique<GLTexture>(path);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: '%s' failed to load; using fallback",
                  path.c_str());
        return fallbackTexture_;
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

ShaderHandle OpenGLRenderer::createShader(const std::string& vertexSrc,
                                          const std::string& fragmentSrc) {
    auto shader = std::make_unique<GLShader>(vertexSrc, fragmentSrc);
    if (!shader->isValid()) {
        Log::error("OpenGLRenderer: shader creation failed");
        return kInvalidHandle;
    }
    shaders_.push_back(std::move(shader));
    return static_cast<ShaderHandle>(shaders_.size());
}

void OpenGLRenderer::beginFrame(Vec3 clearColor, const DirectionalLight& light) {
    light_ = light;
    glClearColor(clearColor.x, clearColor.y, clearColor.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderer::submit(const DrawCall& call, const Mat4& view,
                            const Mat4& projection) {
    // Handles are (index + 1), so a valid handle is in [1, vector size].
    // Reject anything outside that range — a stale or foreign handle would
    // otherwise index a vector out of bounds (undefined behaviour).
    if (call.mesh == kInvalidHandle || call.mesh > meshes_.size() ||
        call.shader == kInvalidHandle || call.shader > shaders_.size()) {
        Log::warn("OpenGLRenderer::submit: mesh/shader handle out of range");
        return;
    }
    const GLShader& shader = *shaders_[call.shader - 1];
    shader.bind();
    shader.setMat4("uModel", call.model);
    shader.setMat4("uView", view);
    shader.setMat4("uProjection", projection);
    shader.setInt("uTexture", 0);
    shader.setVec3("uLightDir", light_.direction);
    shader.setVec3("uLightColor", light_.color);
    shader.setFloat("uAmbient", light_.ambient);

    TextureHandle tex = call.texture;
    if (tex == kInvalidHandle) {
        tex = fallbackTexture_;
    }
    if (tex != kInvalidHandle && tex <= textures_.size()) {
        textures_[tex - 1]->bind(0);
    }

    meshes_[call.mesh - 1]->draw();
}

void OpenGLRenderer::endFrame() {
    // Buffer swap is owned by Window; nothing to flush here yet.
}

void OpenGLRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.addLine(a, b, color);
}

void OpenGLRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    debugLines_.flush(view, projection);
}

void OpenGLRenderer::setViewport(int width, int height) {
    glViewport(0, 0, width, height);
}

} // namespace iron
