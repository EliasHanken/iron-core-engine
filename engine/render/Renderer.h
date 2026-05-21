#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
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
    virtual void beginFrame(Vec3 clearColor) = 0;
    // The camera supplies view + projection; each DrawCall supplies its model.
    virtual void submit(const DrawCall& call, const Mat4& view,
                        const Mat4& projection) = 0;
    virtual void endFrame() = 0;

    // Call when the framebuffer is resized.
    virtual void setViewport(int width, int height) = 0;
};

} // namespace iron
