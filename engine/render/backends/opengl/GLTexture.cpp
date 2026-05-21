#include "render/backends/opengl/GLTexture.h"

#include "core/Log.h"

#include <glad/gl.h>
#include <stb_image.h>

namespace iron {

void GLTexture::uploadRGBA(int width, int height, const unsigned char* rgba) {
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLTexture::GLTexture(int width, int height, const unsigned char* rgba) {
    if (width > 0 && height > 0 && rgba != nullptr) {
        uploadRGBA(width, height, rgba);
    }
}

GLTexture::GLTexture(const std::string& path) {
    stbi_set_flip_vertically_on_load(1);  // OpenGL UV origin is bottom-left
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        Log::error("GLTexture: failed to load '%s'", path.c_str());
        return;
    }
    uploadRGBA(w, h, pixels);
    stbi_image_free(pixels);
}

GLTexture::~GLTexture() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

void GLTexture::bind(int unit) const {
    // Binding an invalid texture would silently attach object 0 and render
    // black with no GL error — warn instead of failing quietly.
    if (!id_) {
        Log::warn("GLTexture::bind called on an invalid texture");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

} // namespace iron
