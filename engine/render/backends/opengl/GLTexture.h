#pragma once

#include <cstdint>
#include <string>

namespace iron {

// A 2D RGBA texture on the GPU. Two ways to build one: from raw RGBA8 pixels,
// or from an image file via stb_image. If construction fails, isValid() is
// false (the renderer substitutes a fallback).
class GLTexture {
public:
    // `rgba` is width*height*4 bytes, row-major.
    GLTexture(int width, int height, const unsigned char* rgba);
    explicit GLTexture(const std::string& path);
    ~GLTexture();

    GLTexture(const GLTexture&) = delete;
    GLTexture& operator=(const GLTexture&) = delete;

    bool isValid() const { return id_ != 0; }
    // Binds to the given texture unit (0, 1, ...).
    void bind(int unit) const;

private:
    void uploadRGBA(int width, int height, const unsigned char* rgba);

    std::uint32_t id_ = 0;
};

} // namespace iron
