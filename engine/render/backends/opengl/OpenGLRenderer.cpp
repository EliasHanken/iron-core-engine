#include "render/backends/opengl/OpenGLRenderer.h"

#include "core/Log.h"
#include "math/Transform.h"

#include <glad/gl.h>

#include <cmath>

namespace {
// The shadow pass renders only depth. The vertex stage transforms by the
// light's view-projection; the fragment stage is empty (GL writes depth).
const char* kDepthVertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uLightViewProj;
uniform mat4 uModel;
void main() {
    gl_Position = uLightViewProj * uModel * vec4(aPos, 1.0);
}
)";

const char* kDepthFragmentSrc = R"(#version 330 core
void main() {}
)";

constexpr int kShadowResolution = 4096;
constexpr float kShadowBias = 0.0005f;
}  // namespace

namespace iron {

OpenGLRenderer::OpenGLRenderer()
    : shadowMap_(kShadowResolution),
      depthShader_(kDepthVertexSrc, kDepthFragmentSrc) {
    glEnable(GL_DEPTH_TEST);

    // A 2x2 magenta/black checker used when a real texture fails to load.
    const unsigned char fallback[16] = {
        255, 0, 255, 255,   0, 0, 0, 255,
        0, 0, 0, 255,       255, 0, 255, 255,
    };
    fallbackTexture_ = createTexture(2, 2, fallback);

    // A 1x1 opaque-white texture so solid-colour HUD quads can reuse the
    // textured HUD shader (sample white, tint by vertex colour).
    const unsigned char white[4] = {255, 255, 255, 255};
    whiteTexture_ = createTexture(1, 1, white);
}

MeshHandle OpenGLRenderer::createMesh(const MeshData& data) {
    meshes_.push_back(std::make_unique<GLMesh>(data));
    return static_cast<MeshHandle>(meshes_.size());  // index + 1
}

void OpenGLRenderer::updateMesh(MeshHandle mesh, const MeshData& data) {
    // Handles are (index + 1); reject anything out of range.
    if (mesh == kInvalidHandle || mesh > meshes_.size()) {
        Log::warn("OpenGLRenderer::updateMesh: mesh handle out of range");
        return;
    }
    meshes_[mesh - 1]->update(data);
}

TextureHandle OpenGLRenderer::createTexture(int width, int height,
                                            const unsigned char* rgba) {
    auto tex = std::make_unique<GLTexture>(width, height, rgba);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: createTexture produced an invalid texture");
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

TextureHandle OpenGLRenderer::whiteTexture() const {
    return whiteTexture_;
}

TextureHandle OpenGLRenderer::loadTexture(const std::string& path) {
    auto tex = std::make_unique<GLTexture>(path);
    if (!tex->isValid()) {
        Log::warn("OpenGLRenderer: '%s' failed to load; using fallback",
                  path.c_str());
        return fallbackTexture_;
    }
    textures_.push_back(std::move(tex));
    return static_cast<TextureHandle>(textures_.size());
}

ShaderHandle OpenGLRenderer::createShader(const std::string& vertexSrc,
                                          const std::string& fragmentSrc) {
    auto shader = std::make_unique<GLShader>(vertexSrc, fragmentSrc);
    if (!shader->isValid()) {
        Log::error("OpenGLRenderer: shader creation failed");
        return kInvalidHandle;
    }
    shaders_.push_back(std::move(shader));
    return static_cast<ShaderHandle>(shaders_.size());
}

void OpenGLRenderer::beginFrame(Vec3 clearColor, const DirectionalLight& light,
                                const Mat4& view, const Mat4& projection) {
    clearColor_ = clearColor;
    light_ = light;
    view_ = view;
    projection_ = projection;
    frameCalls_.clear();
}

void OpenGLRenderer::submit(const DrawCall& call) {
    // Handles are (index + 1), so a valid handle is in [1, vector size].
    // Reject anything outside that range — a stale or foreign handle would
    // otherwise index a vector out of bounds (undefined behaviour).
    if (call.mesh == kInvalidHandle || call.mesh > meshes_.size() ||
        call.shader == kInvalidHandle || call.shader > shaders_.size()) {
        Log::warn("OpenGLRenderer::submit: mesh/shader handle out of range");
        return;
    }
    frameCalls_.push_back(call);
}

void OpenGLRenderer::setShadowBounds(Vec3 center, float radius) {
    shadowCenter_ = center;
    shadowRadius_ = radius;
}

Mat4 OpenGLRenderer::computeLightViewProj() const {
    // The directional light's "camera": an orthographic box aimed along the
    // light direction, sized to enclose the shadow bounds sphere.
    Vec3 dir = normalize(light_.direction);
    const Vec3 up = (std::fabs(dir.y) > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f}
                                               : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 eye = shadowCenter_ - dir * (shadowRadius_ * 2.0f);
    const Mat4 view = lookAt(eye, shadowCenter_, up);
    const Mat4 proj = orthographic(-shadowRadius_, shadowRadius_,
                                   -shadowRadius_, shadowRadius_,
                                   shadowRadius_ * 0.5f, shadowRadius_ * 3.5f);
    return proj * view;
}

void OpenGLRenderer::endFrame() {
    const Mat4 lightViewProj = computeLightViewProj();

    // --- Pass 1: render scene depth from the light into the shadow map ---
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);

    shadowMap_.bindForWriting();
    glViewport(0, 0, shadowMap_.resolution(), shadowMap_.resolution());
    glClear(GL_DEPTH_BUFFER_BIT);
    depthShader_.bind();
    depthShader_.setMat4("uLightViewProj", lightViewProj);
    for (const DrawCall& call : frameCalls_) {
        depthShader_.setMat4("uModel", call.model);
        meshes_[call.mesh - 1]->draw();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2],
               savedViewport[3]);

    // --- Pass 2: the lit scene, sampling the shadow map ---
    glClearColor(clearColor_.x, clearColor_.y, clearColor_.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (const DrawCall& call : frameCalls_) {
        const GLShader& shader = *shaders_[call.shader - 1];
        shader.bind();
        shader.setMat4("uModel", call.model);
        shader.setMat4("uView", view_);
        shader.setMat4("uProjection", projection_);
        shader.setMat4("uLightViewProj", lightViewProj);
        shader.setInt("uTexture", 0);
        shader.setInt("uShadowMap", 1);
        shader.setFloat("uShadowBias", kShadowBias);
        shader.setVec3("uLightDir", light_.direction);
        shader.setVec3("uLightColor", light_.color);
        shader.setFloat("uAmbient", light_.ambient);

        TextureHandle tex = call.texture;
        if (tex == kInvalidHandle) {
            tex = fallbackTexture_;
        }
        if (tex != kInvalidHandle && tex <= textures_.size()) {
            textures_[tex - 1]->bind(0);
        }
        shadowMap_.bindDepthTexture(1);

        meshes_[call.mesh - 1]->draw();
    }

    // Leave unit 1 unbound — the shadow map is the renderer's; overlays
    // (HUD, debug lines) must not inherit it. Leave unit 0 active.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

void OpenGLRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.addLine(a, b, color);
}

void OpenGLRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    debugLines_.flush(view, projection);
}

void OpenGLRenderer::drawHud(const HudBatch& batch, int framebufferWidth,
                             int framebufferHeight) {
    if (batch.empty()) {
        return;
    }

    // HUD draws on top of everything: no depth test, alpha-blended.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    hud_.begin(framebufferWidth, framebufferHeight);
    for (const HudDrawGroup& group : batch) {
        if (group.vertices.empty()) {
            continue;
        }
        // Bind the group's texture to unit 0; fall back to white if invalid.
        TextureHandle tex = group.texture;
        if (tex == kInvalidHandle || tex > textures_.size()) {
            tex = whiteTexture_;
        }
        if (tex != kInvalidHandle && tex <= textures_.size()) {
            textures_[tex - 1]->bind(0);
        }
        hud_.drawGroup(group.vertices);
    }
    hud_.end();

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void OpenGLRenderer::setViewport(int width, int height) {
    glViewport(0, 0, width, height);
}

} // namespace iron
