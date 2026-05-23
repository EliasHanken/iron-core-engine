#include "render/backends/opengl/GLCubemap.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

GLCubemap::GLCubemap(int width, int height,
                     const std::array<const unsigned char*, 6>& faces) {
    // Validate: any null face → invalid cubemap.
    for (const unsigned char* face : faces) {
        if (face == nullptr) {
            Log::error("GLCubemap: null face data; cubemap will be invalid");
            return;
        }
    }
    if (width <= 0 || height <= 0) {
        Log::error("GLCubemap: invalid dimensions %dx%d", width, height);
        return;
    }
    if (width != height) {
        Log::error("GLCubemap: faces must be square, got %dx%d", width, height);
        return;
    }

    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id_);

    // GL face targets are contiguous (+X, -X, +Y, -Y, +Z, -Z).
    for (int i = 0; i < 6; ++i) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA,
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     faces[i]);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

GLCubemap::~GLCubemap() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

void GLCubemap::bind(int unit) const {
    if (!id_) {
        Log::warn("GLCubemap::bind called on an invalid cubemap");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id_);
}

} // namespace iron
