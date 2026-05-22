#include "render/backends/opengl/GLShadowMap.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

GLShadowMap::GLShadowMap(int resolution) : resolution_(resolution) {
    // Depth texture. CLAMP_TO_BORDER with a white (1.0) border so samples
    // outside the map read as "fully lit / farthest".
    glGenTextures(1, &depthTexture_);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, resolution,
                 resolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Framebuffer with only the depth attachment — no colour buffer.
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           depthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        complete_ = true;
    } else {
        Log::error("GLShadowMap: framebuffer is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLShadowMap::~GLShadowMap() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (depthTexture_) glDeleteTextures(1, &depthTexture_);
}

void GLShadowMap::bindForWriting() const {
    if (!complete_) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void GLShadowMap::bindDepthTexture(int unit) const {
    if (!complete_) {
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
}

} // namespace iron
