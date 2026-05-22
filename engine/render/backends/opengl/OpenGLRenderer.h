#pragma once

#include "render/Renderer.h"
#include "render/backends/opengl/GLDebugLines.h"
#include "render/backends/opengl/GLHud.h"
#include "render/backends/opengl/GLMesh.h"
#include "render/backends/opengl/GLShader.h"
#include "render/backends/opengl/GLTexture.h"

#include <memory>
#include <vector>

namespace iron {

// OpenGL 3.3 implementation of the RHI. Resources live in vectors; handles are
// (index + 1) so 0 stays the invalid handle. Requires a current GL context
// (create a Window first).
class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer();

    MeshHandle createMesh(const MeshData& data) override;
    void updateMesh(MeshHandle mesh, const MeshData& data) override;
    TextureHandle createTexture(int width, int height,
                                const unsigned char* rgba) override;
    TextureHandle loadTexture(const std::string& path) override;
    TextureHandle whiteTexture() const override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;

    void beginFrame(Vec3 clearColor, const DirectionalLight& light) override;
    void submit(const DrawCall& call, const Mat4& view,
                const Mat4& projection) override;
    void endFrame() override;

    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;

    void setViewport(int width, int height) override;

private:
    std::vector<std::unique_ptr<GLMesh>> meshes_;
    std::vector<std::unique_ptr<GLTexture>> textures_;
    std::vector<std::unique_ptr<GLShader>> shaders_;
    TextureHandle fallbackTexture_ = kInvalidHandle;
    DirectionalLight light_{};
    GLDebugLines debugLines_;
    GLHud hud_;
    TextureHandle whiteTexture_ = kInvalidHandle;
};

} // namespace iron
