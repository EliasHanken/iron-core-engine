#pragma once

#include <cstdint>

namespace iron {

// Render target for the planar reflection pass: an FBO with an RGBA8
// colour texture and a 24-bit depth texture, both at the same square
// resolution. The depth texture exists so geometry depth-tests
// correctly within the reflection scene.
class GLReflectionTarget {
public:
    explicit GLReflectionTarget(int resolution);
    ~GLReflectionTarget();

    GLReflectionTarget(const GLReflectionTarget&) = delete;
    GLReflectionTarget& operator=(const GLReflectionTarget&) = delete;

    bool isValid() const { return fbo_ != 0 && complete_; }
    int resolution() const { return resolution_; }

    // Binds the FBO for writing. The caller is responsible for setting
    // the viewport and clearing colour/depth.
    void bindForWriting() const;

    // Binds the colour texture to the given texture unit so the lit
    // shader can sample it.
    void bindColorTexture(int unit) const;

private:
    std::uint32_t fbo_ = 0;
    std::uint32_t colorTexture_ = 0;
    std::uint32_t depthTexture_ = 0;
    int resolution_ = 0;
    bool complete_ = false;
};

} // namespace iron
