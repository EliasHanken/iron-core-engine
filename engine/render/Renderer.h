#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/Light.h"
#include "scene/Mesh.h"

#include <cstdint>
#include <string>

namespace iron {

// Opaque handles into the renderer's resource tables. 0 is "invalid".
using MeshHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderHandle = std::uint32_t;

inline constexpr std::uint32_t kInvalidHandle = 0;

// One thing to draw: a mesh, a shader, a texture, and a model matrix.
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
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
    // RGBA8 pixels, `width * height * 4` bytes, row-major from top-left.
    virtual TextureHandle createTexture(int width, int height,
                                        const unsigned char* rgba) = 0;
    // Loads an image file (PNG/JPG) into a texture. Returns kInvalidHandle on
    // failure.
    virtual TextureHandle loadTexture(const std::string& path) = 0;
    // Compiles + links a shader from GLSL source. Returns kInvalidHandle on
    // failure.
    virtual ShaderHandle createShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) = 0;

    // --- per-frame ---
    // The directional light applies to every object drawn this frame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light) = 0;
    // The camera supplies view + projection; each DrawCall supplies its model.
    virtual void submit(const DrawCall& call, const Mat4& view,
                        const Mat4& projection) = 0;
    virtual void endFrame() = 0;

    // --- debug drawing ---
    // Queue a coloured 3D line segment for the current frame.
    virtual void drawLine(Vec3 a, Vec3 b, Vec3 color) = 0;
    // Draw all queued debug lines (depth-tested) and clear the queue.
    virtual void flushDebugLines(const Mat4& view, const Mat4& projection) = 0;

    // Call when the framebuffer is resized.
    virtual void setViewport(int width, int height) = 0;
};

} // namespace iron
