#include "render/backends/opengl/GLReflectionTarget.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

GLReflectionTarget::GLReflectionTarget(int resolution)
    : resolution_(resolution) {
    if (resolution <= 0) {
        Log::error("GLReflectionTarget: invalid resolution %d", resolution);
        return;
    }

    // Colour texture (RGBA8).
    glGenTextures(1, &colorTexture_);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, resolution, resolution, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Depth texture (24-bit).
    glGenTextures(1, &depthTexture_);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 resolution, resolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // FBO.
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, colorTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    complete_ = (status == GL_FRAMEBUFFER_COMPLETE);
    if (!complete_) {
        Log::error("GLReflectionTarget: framebuffer incomplete (0x%x)",
                   status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLReflectionTarget::~GLReflectionTarget() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (colorTexture_) glDeleteTextures(1, &colorTexture_);
    if (depthTexture_) glDeleteTextures(1, &depthTexture_);
}

void GLReflectionTarget::bindForWriting() const {
    if (!isValid()) {
        Log::warn("GLReflectionTarget::bindForWriting on invalid target");
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void GLReflectionTarget::bindColorTexture(int unit) const {
    if (!isValid()) {
        Log::warn("GLReflectionTarget::bindColorTexture on invalid target");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
}

} // namespace iron
