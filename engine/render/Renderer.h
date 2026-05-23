#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/Handles.h"
#include "render/HudBatch.h"
#include "render/Light.h"
#include "scene/Mesh.h"

#include <string>

namespace iron {

// Maximum point lights uploaded to the lit shader per frame. The lit
// fragment shader declares a uniform array of this size. Extras passed
// to beginFrame are silently dropped (and logged once per frame in debug).
constexpr int kMaxPointLights = 16;

// One thing to draw: a mesh, a shader, a texture, and a model matrix.
// `emissive` is added on top of lighting in the lit fragment shader —
// use it for visible light sources (lantern bulbs, glowing crystals).
// Default (0,0,0) means "no glow", indistinguishable from before.
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Vec3 emissive{0.0f, 0.0f, 0.0f};
};

// Render Hardware Interface: a graphics-API-agnostic renderer. Game code talks
// only to this interface; concrete backends (OpenGLRenderer today, others
// later) implement it. Keeping this surface small is deliberate — it is the
// contract every future backend must honour.
//
// Resource lifetime: resources created here live until the Renderer is
// destroyed (application-scoped). There is intentionally no per-resource
// destroy API yet — explicit destruction lands when a game needs to stream
// resources, and will be added as a deliberate interface change then.
class Renderer {
public:
    virtual ~Renderer() = default;

    // --- resource creation ---
    virtual MeshHandle createMesh(const MeshData& data) = 0;
    // Replace the geometry of an existing mesh (for meshes rebuilt per frame).
    virtual void updateMesh(MeshHandle mesh, const MeshData& data) = 0;
    // RGBA8 pixels, `width * height * 4` bytes, row-major from top-left.
    virtual TextureHandle createTexture(int width, int height,
                                        const unsigned char* rgba) = 0;
    // Loads an image file (PNG/JPG) into a texture. Returns kInvalidHandle on
    // failure.
    virtual TextureHandle loadTexture(const std::string& path) = 0;
    // A built-in 1x1 opaque-white texture, handy for solid-colour quads.
    virtual TextureHandle whiteTexture() const = 0;
    // Compiles + links a shader from GLSL source. Returns kInvalidHandle on
    // failure.
    virtual ShaderHandle createShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) = 0;

    // --- per-frame ---
    // Begins a frame: records the clear colour, the directional light, and the
    // camera (view + projection). Submitted draw calls are buffered until
    // endFrame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                            const Mat4& view, const Mat4& projection) = 0;
    // Records one draw call for this frame. Each DrawCall supplies its model
    // matrix; the camera comes from beginFrame.
    virtual void submit(const DrawCall& call) = 0;
    // Renders every buffered draw call for the frame.
    virtual void endFrame() = 0;

    // Sets the world-space sphere (centre + radius) the directional light's
    // shadow map must cover. A game calls this once with bounds enclosing its
    // scene.
    virtual void setShadowBounds(Vec3 center, float radius) = 0;

    // --- debug drawing ---
    // Queue a coloured 3D line segment for the current frame.
    virtual void drawLine(Vec3 a, Vec3 b, Vec3 color) = 0;
    // Draw all queued debug lines (depth-tested) and clear the queue.
    virtual void flushDebugLines(const Mat4& view, const Mat4& projection) = 0;

    // --- HUD ---
    // Draw a screen-space HUD batch on top of the finished frame, sized to the
    // given framebuffer dimensions. Call after endFrame (the HUD is an overlay
    // composited on top of the rendered scene).
    virtual void drawHud(const HudBatch& batch, int framebufferWidth,
                         int framebufferHeight) = 0;

    // Call when the framebuffer is resized.
    virtual void setViewport(int width, int height) = 0;
};

} // namespace iron
