#pragma once

#include "render/Renderer.h"
#include "render/backends/opengl/GLDebugLines.h"
#include "render/backends/opengl/GLHud.h"
#include "render/backends/opengl/GLMesh.h"
#include "render/backends/opengl/GLShader.h"
#include "render/backends/opengl/GLShadowMap.h"
#include "render/backends/opengl/GLTexture.h"

#include <memory>
#include <span>
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

    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;
    void setShadowBounds(Vec3 center, float radius) override;

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
    std::vector<PointLight> pointLights_;
    std::vector<DrawCall> frameCalls_;
    Vec3 clearColor_{};
    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    GLDebugLines debugLines_;
    GLHud hud_;
    TextureHandle whiteTexture_ = kInvalidHandle;
    GLShadowMap shadowMap_;
    GLShader depthShader_;
    Vec3 shadowCenter_{0.0f, 0.0f, 0.0f};
    float shadowRadius_ = 50.0f;

    Mat4 computeLightViewProj() const;
};

} // namespace iron
