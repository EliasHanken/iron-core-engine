#pragma once

#include "render/Renderer.h"
#include "render/backends/opengl/GLCubemap.h"
#include "render/backends/opengl/GLDebugLines.h"
#include "render/backends/opengl/GLSkybox.h"
#include "render/backends/opengl/GLHud.h"
#include "render/backends/opengl/GLMesh.h"
#include "render/backends/opengl/GLShader.h"
#include "render/backends/opengl/GLReflectionTarget.h"
#include "render/backends/opengl/GLShadowMap.h"
#include "render/backends/opengl/GLTexture.h"

#include <array>
#include <memory>
#include <optional>
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
                                const unsigned char* rgba,
                                bool srgb = true) override;
    TextureHandle loadTexture(const std::string& path,
                              bool srgb = true) override;
    TextureHandle whiteTexture() const override;
    TextureHandle flatNormalTexture() const override;
    TextureHandle noSpecularTexture() const override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) override;
    ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) override;
    // M50b — tessellation is Vulkan-only; stub returns invalid handle.
    ShaderHandle createTessellatedShader(const std::string&, const std::string&,
                                          const std::string&, const std::string&) override {
        return kInvalidHandle;
    }
    bool reloadShader(ShaderHandle, const std::string&, const std::string&) override;
    void submitSkinnedDraw(const SkinnedDrawCall& call) override;
    CubemapHandle createCubemap(
        int width, int height,
        const std::array<const unsigned char*, 6>& faces) override;
    void setSkybox(CubemapHandle sky) override;
    CubemapHandle loadHdrSkybox(const std::string& hdrPath, int faceSize) override;
    void setReflectionProbes(std::span<const GpuReflectionProbe>) override {}
    void bakeReflectionProbes(std::vector<GpuReflectionProbe>&) override {}

    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Fog& fog,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;
    void setShadowBounds(Vec3 center, float radius) override;
    void setReflectionPlane(Vec3 normal, float d) override;
    void disableReflectionPlane() override;
    // M50b — wireframe + tessellation are Vulkan-only; stubs.
    void setWireframe(bool) override {}
    void setTessellationFactor(float) override {}

    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;

    void setViewport(int width, int height) override;

private:
    std::vector<std::unique_ptr<GLMesh>> meshes_;
    std::vector<std::unique_ptr<GLTexture>> textures_;
    std::vector<std::unique_ptr<GLShader>> shaders_;
    std::vector<std::unique_ptr<GLCubemap>> cubemaps_;
    CubemapHandle skybox_ = kInvalidHandle;
    TextureHandle fallbackTexture_ = kInvalidHandle;
    DirectionalLight light_{};
    std::vector<PointLight> pointLights_;
    Fog fog_{};
    std::vector<DrawCall> frameCalls_;
    Vec3 clearColor_{};
    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    GLDebugLines debugLines_;
    GLHud hud_;
    GLSkybox skybox_pass_;
    TextureHandle whiteTexture_ = kInvalidHandle;
    TextureHandle flatNormalTexture_ = kInvalidHandle;
    TextureHandle noSpecularTexture_ = kInvalidHandle;
    GLShadowMap shadowMap_;
    GLShader depthShader_;
    GLReflectionTarget reflectionTarget_;
    GLShader reflectionShader_;
    Vec3 shadowCenter_{0.0f, 0.0f, 0.0f};
    float shadowRadius_ = 50.0f;
    std::optional<ReflectionPlane> reflectionPlane_;
    Vec3 cameraPos_{};
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;

    // M11 — HudBatch is buffered at drawHud time and emitted at the
    // tail of endFrame, after the lit/reflection/shadow passes finish
    // writing the default framebuffer. Matches the cross-backend
    // contract: drawHud is called before endFrame.
    HudBatch  pendingHudBatch_{};
    int       pendingHudWidth_  = 0;
    int       pendingHudHeight_ = 0;
    bool      pendingHudValid_  = false;

    // M11 — like HUD, debug-line flushing is deferred until endFrame so
    // the lines land ON TOP of the lit pass instead of being overwritten
    // by it.
    Mat4  pendingDebugView_       = Mat4::identity();
    Mat4  pendingDebugProjection_ = Mat4::identity();
    bool  pendingDebugFlush_      = false;

    Mat4 computeLightViewProj() const;
};

} // namespace iron
