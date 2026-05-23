#pragma once

#include <array>
#include <cstdint>

namespace iron {

// Wraps a GL_TEXTURE_CUBE_MAP. Six RGBA8 face textures (all the same
// size, square); GL_CLAMP_TO_EDGE on S/T/R; linear min/mag filtering.
// Face order matches OpenGL: +X, -X, +Y, -Y, +Z, -Z.
class GLCubemap {
public:
    // Uploads six faces. Each face is `width * height * 4` bytes RGBA.
    // If any face pointer is null the cubemap stays invalid.
    GLCubemap(int width, int height,
              const std::array<const unsigned char*, 6>& faces);
    ~GLCubemap();

    GLCubemap(const GLCubemap&) = delete;
    GLCubemap& operator=(const GLCubemap&) = delete;

    bool isValid() const { return id_ != 0; }
    void bind(int unit) const;

private:
    std::uint32_t id_ = 0;
};

} // namespace iron
