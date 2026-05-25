#pragma once

#include "render/Renderer.h"

#include <unordered_map>
#include <unordered_set>
#include <string>

namespace iron {

class Window;

// Vulkan backend for iron::Renderer. Built only when
// IRON_RENDER_BACKEND_VULKAN is defined.
//
// M9 scope: brings up the foundation (instance, device, swapchain,
// frame ring, opaque mesh+texture+shader, single render pass) and
// runs games/01-spinning-cube. Stubbed methods (cubemap, skybox,
// shadow, reflection, debug lines, HUD) log a one-time warning and
// return safely; they land in subsequent milestones.
class VulkanRenderer : public Renderer {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

    bool init(Window& window);
    bool initOk() const { return initOk_; }

    // --- resource creation ---
    MeshHandle createMesh(const MeshData& data) override;
    void updateMesh(MeshHandle mesh, const MeshData& data) override;
    TextureHandle createTexture(int width, int height,
                                const unsigned char* rgba) override;
    TextureHandle loadTexture(const std::string& path) override;
    TextureHandle whiteTexture() const override;
    TextureHandle flatNormalTexture() const override;
    TextureHandle noSpecularTexture() const override;
    ShaderHandle createShader(const std::string& vertexSrc,
                              const std::string& fragmentSrc) override;
    CubemapHandle createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) override;
    void setSkybox(CubemapHandle sky) override;

    // --- per-frame ---
    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    std::span<const PointLight> pointLights,
                    const Fog& fog,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;

    void setShadowBounds(Vec3 center, float radius) override;
    void setReflectionPlane(Vec3 normal, float d) override;
    void disableReflectionPlane() override;

    // --- debug ---
    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;

    // --- HUD ---
    void drawHud(const HudBatch& batch, int framebufferWidth,
                 int framebufferHeight) override;

    void setViewport(int width, int height) override;

private:
    void warnOnce(const char* feature);

    bool initOk_ = false;
    std::unordered_set<std::string> warnedFeatures_;
};

}  // namespace iron
