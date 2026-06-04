// tests/MockRenderer.h — header-only no-op iron::Renderer for tests that
// only care about drawLine calls. Add overrides for other methods as new
// tests need them.

#pragma once

#include "render/Renderer.h"

#include <string>
#include <vector>

namespace iron {

struct CapturedLine {
    Vec3 a;
    Vec3 b;
    Vec3 color;
};

class MockRenderer : public Renderer {
public:
    std::vector<CapturedLine> lines;

    // Resource creation — return invalid handles; tests don't exercise these.
    MeshHandle createMesh(const MeshData&) override { return kInvalidHandle; }
    void updateMesh(MeshHandle, const MeshData&) override {}
    TextureHandle createTexture(int, int, const unsigned char*, bool = true) override { return kInvalidHandle; }
    TextureHandle loadTexture(const std::string&, bool = true) override { return kInvalidHandle; }
    TextureHandle whiteTexture() const override { return kInvalidHandle; }
    TextureHandle flatNormalTexture() const override { return kInvalidHandle; }
    TextureHandle noSpecularTexture() const override { return kInvalidHandle; }
    ShaderHandle createShader(const std::string&, const std::string&) override { return kInvalidHandle; }
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) override { return kInvalidSkinnedMesh; }
    ShaderHandle createSkinnedShader(const std::string&, const std::string&) override { return kInvalidHandle; }
    bool reloadShader(ShaderHandle, const std::string&, const std::string&) override { return true; }
    void submitSkinnedDraw(const SkinnedDrawCall&) override {}
    CubemapHandle createCubemap(int, int, const std::array<const unsigned char*, 6>&) override { return kInvalidHandle; }
    void setSkybox(CubemapHandle) override {}
    CubemapHandle loadHdrSkybox(const std::string&, int) override { return kInvalidHandle; }
    void setReflectionProbes(std::span<const GpuReflectionProbe>) override {}
    void bakeReflectionProbes(std::vector<GpuReflectionProbe>&) override {}

    // Per-frame — no-op.
    void beginFrame(Vec3, const DirectionalLight&,
                    std::span<const PointLight>, const Fog&,
                    const Mat4&, const Mat4&) override {}
    void submit(const DrawCall&) override {}
    void endFrame() override {}

    void setShadowBounds(Vec3, float) override {}
    void setReflectionPlane(Vec3, float) override {}
    void disableReflectionPlane() override {}

    // Debug drawing — drawLine captures; flushDebugLines no-op (the registry
    // doesn't call it).
    void drawLine(Vec3 a, Vec3 b, Vec3 color) override {
        lines.push_back({a, b, color});
    }
    void flushDebugLines(const Mat4&, const Mat4&) override {}

    void drawHud(const HudBatch&, int, int) override {}
    void setViewport(int, int) override {}
};

}  // namespace iron
