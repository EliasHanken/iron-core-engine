#pragma once

#include <cstdint>

namespace iron {

// A depth-only framebuffer for shadow mapping: a framebuffer object with a
// single depth-texture attachment (no colour buffer). Render the scene's depth
// into it from the light's viewpoint, then sample the depth texture in the lit
// pass. Square, `resolution` x `resolution`. Requires a current GL context.
class GLShadowMap {
public:
    explicit GLShadowMap(int resolution);
    ~GLShadowMap();

    GLShadowMap(const GLShadowMap&) = delete;
    GLShadowMap& operator=(const GLShadowMap&) = delete;

    // Bind the framebuffer as the current render target. The caller then sets
    // the viewport to resolution() and clears the depth buffer.
    void bindForWriting() const;

    // Bind the depth texture to texture unit `unit` for sampling.
    void bindDepthTexture(int unit) const;

    int resolution() const { return resolution_; }

    // False if the framebuffer failed to construct (incomplete). The bind
    // methods are no-ops on an invalid shadow map.
    bool isValid() const { return complete_; }

private:
    int resolution_ = 0;
    std::uint32_t fbo_ = 0;
    std::uint32_t depthTexture_ = 0;
    bool complete_ = false;
};

} // namespace iron
