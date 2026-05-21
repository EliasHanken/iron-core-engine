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

void OpenGLRenderer::beginFrame(Vec3 clearColor) {
    glClearColor(clearColor.x, clearColor.y, clearColor.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderer::submit(const DrawCall& call, const Mat4& view,
                            const Mat4& projection) {
    if (call.mesh == kInvalidHandle || call.shader == kInvalidHandle) {
        return;
    }
    const GLShader& shader = *shaders_[call.shader - 1];
    shader.bind();
    shader.setMat4("uModel", call.model);
    shader.setMat4("uView", view);
    shader.setMat4("uProjection", projection);
    shader.setInt("uTexture", 0);

    TextureHandle tex = call.texture;
    if (tex == kInvalidHandle) {
        tex = fallbackTexture_;
    }
    if (tex != kInvalidHandle) {
        textures_[tex - 1]->bind(0);
    }

    meshes_[call.mesh - 1]->draw();
}

void OpenGLRenderer::endFrame() {
    // Buffer swap is owned by Window; nothing to flush here yet.
}

void OpenGLRenderer::setViewport(int width, int height) {
    glViewport(0, 0, width, height);
}

} // namespace iron
