# Shadow Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the engine real-time shadows — the directional light casts hard shadows from the islands, props, pole, and ropes.

**Architecture:** The renderer becomes buffered (`submit` records draw calls; `endFrame` replays them). `endFrame` runs two passes: a depth pass from the light into a shadow-map framebuffer, then the lit pass sampling that depth. The math library gains `orthographic()` for the directional light's projection.

**Tech Stack:** C++23, OpenGL 3.3 via the engine RHI, CMake, MSVC, the project's custom CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-22-shadow-mapping-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `engine/math/Transform.h` (modified) | Adds `orthographic()`. |
| `engine/render/backends/opengl/GLShadowMap.h/.cpp` (new) | A depth-only framebuffer object — the shadow map. |
| `engine/render/Renderer.h` (modified) | `beginFrame`/`submit` signatures change for buffering; adds `setShadowBounds`. |
| `engine/render/backends/opengl/OpenGLRenderer.h/.cpp` (modified) | Buffers draw calls; `endFrame` runs the shadow pass then the lit pass. |
| `engine/CMakeLists.txt` (modified) | Registers `GLShadowMap.cpp`. |
| `games/02-strandbound/RopeTool.h/.cpp` (modified) | `draw()` loses its view/projection parameters (the new `submit` doesn't take them). |
| `games/02-strandbound/main.cpp` (modified) | New `beginFrame`/`submit` calls; the render lambda reorders; the shadow-aware lit shader; a `setShadowBounds` call. |
| `tests/test_transform.cpp` (modified) | `orthographic()` test. |
| `docs/engine/shadow-mapping.md` (new) | Concept note. |

**Task ordering:** Task 1 and Task 2 are additive (build stays green). Task 3 changes the `beginFrame`/`submit` RHI signatures and updates their only callers (`main.cpp`, `RopeTool`) together — a buffered renderer with a single lit pass and *no behaviour change*. Task 4 adds the shadow pass on top. Each task leaves a green build and a runnable game.

---

## Task 1: `orthographic()` in the math library

The directional light's shadow projection. A pure function, unit-tested.

**Files:**
- Modify: `engine/math/Transform.h`
- Modify: `tests/test_transform.cpp`

- [ ] **Step 1: Write the failing test — add to `tests/test_transform.cpp`**

Add this block inside `main()`, just before `return iron_test_result();`:

```cpp
    // orthographic maps the box to NDC. Box: x,y in [-10,10], near=1,
    // far=100; view space looks down -Z.
    {
        Mat4 ortho = orthographic(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 100.0f);
        // Centre of the box maps to the NDC origin in x and y.
        Vec4 centre = ortho * Vec4{0.0f, 0.0f, -50.5f, 1.0f};
        CHECK_NEAR(centre.x, 0.0f);
        CHECK_NEAR(centre.y, 0.0f);
        // Right/top edge maps to +1, +1.
        Vec4 corner = ortho * Vec4{10.0f, 10.0f, -1.0f, 1.0f};
        CHECK_NEAR(corner.x, 1.0f);
        CHECK_NEAR(corner.y, 1.0f);
        // The near plane maps to NDC z = -1, the far plane to z = +1.
        Vec4 nearP = ortho * Vec4{0.0f, 0.0f, -1.0f, 1.0f};
        CHECK_NEAR(nearP.z, -1.0f);
        Vec4 farP = ortho * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
        CHECK_NEAR(farP.z, 1.0f);
        // Orthographic keeps w = 1 (no perspective divide).
        CHECK_NEAR(corner.w, 1.0f);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: build FAILS — `orthographic` is not declared.

- [ ] **Step 3: Add `orthographic()` to `engine/math/Transform.h`**

Add this function directly after the existing `perspective` function, before the closing `} // namespace iron`:

```cpp
// Orthographic projection. Maps the axis-aligned box [left,right] x
// [bottom,top] x [-far,-near] (right-handed, looking down -Z) into OpenGL clip
// space with every axis in [-1, 1]. Used for a directional light's shadow
// projection. Matches the convention of lookAt / perspective.
inline Mat4 orthographic(float left, float right, float bottom, float top,
                         float nearZ, float farZ) {
    Mat4 m;  // all zeros
    m.at(0, 0) = 2.0f / (right - left);
    m.at(1, 1) = 2.0f / (top - bottom);
    m.at(2, 2) = -2.0f / (farZ - nearZ);
    m.at(0, 3) = -(right + left) / (right - left);
    m.at(1, 3) = -(top + bottom) / (top - bottom);
    m.at(2, 3) = -(farZ + nearZ) / (farZ - nearZ);
    m.at(3, 3) = 1.0f;
    return m;
}
```

- [ ] **Step 4: Build and run the test**

Run: `cmake --build build` then
`ctest --test-dir build -C Debug -R test_transform --output-on-failure`
Expected: `test_transform` passes.

- [ ] **Step 5: Commit**

```bash
git add engine/math/Transform.h tests/test_transform.cpp
git commit -m "$(cat <<'EOF'
Add orthographic() projection builder

A column-major orthographic projection matrix — the directional light's
shadow-map projection. Pure, unit-tested alongside perspective/lookAt.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `GLShadowMap` — a depth framebuffer

The engine's first render target: a framebuffer object with a single depth-texture attachment. Additive — nothing uses it yet, so the build stays green. No unit test (GL code; verified once it is used).

**Files:**
- Create: `engine/render/backends/opengl/GLShadowMap.h`
- Create: `engine/render/backends/opengl/GLShadowMap.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Create `engine/render/backends/opengl/GLShadowMap.h`**

```cpp
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

private:
    int resolution_;
    std::uint32_t fbo_ = 0;
    std::uint32_t depthTexture_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Create `engine/render/backends/opengl/GLShadowMap.cpp`**

```cpp
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
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Log::error("GLShadowMap: framebuffer is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLShadowMap::~GLShadowMap() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (depthTexture_) glDeleteTextures(1, &depthTexture_);
}

void GLShadowMap::bindForWriting() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void GLShadowMap::bindDepthTexture(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
}

} // namespace iron
```

- [ ] **Step 3: Register `GLShadowMap.cpp` in `engine/CMakeLists.txt`**

Add `render/backends/opengl/GLShadowMap.cpp` to the `add_library(ironcore STATIC ...)` source list, after `render/backends/opengl/GLHud.cpp`:

```cmake
  render/backends/opengl/GLHud.cpp
  render/backends/opengl/GLShadowMap.cpp
```

- [ ] **Step 4: Build and run the test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: clean build; all 12 tests pass (`GLShadowMap` compiles into the library but is not used yet).

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/opengl/GLShadowMap.h engine/render/backends/opengl/GLShadowMap.cpp engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add GLShadowMap — a depth framebuffer

A framebuffer object with a single depth-texture attachment: the
render target for the shadow pass.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Buffer the renderer (single lit pass, no behaviour change)

Make `submit` record draw calls and `endFrame` replay them. This is a pure refactor — the renderer still does exactly one lit pass with today's lighting, so the game looks identical. It changes the `beginFrame`/`submit` RHI signatures, so their callers (`main.cpp`, `RopeTool`) are updated in the same task to keep the build green.

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/02-strandbound/RopeTool.h`
- Modify: `games/02-strandbound/RopeTool.cpp`
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Change the `beginFrame` / `submit` signatures in `engine/render/Renderer.h`**

In `class Renderer`, the `--- per-frame ---` group currently is:

```cpp
    // --- per-frame ---
    // The directional light applies to every object drawn this frame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light) = 0;
    // The camera supplies view + projection; each DrawCall supplies its model.
    virtual void submit(const DrawCall& call, const Mat4& view,
                        const Mat4& projection) = 0;
    virtual void endFrame() = 0;
```

Replace it with:

```cpp
    // --- per-frame ---
    // Begins a frame: records the clear colour, the directional light, and the
    // camera (view + projection). Submitted draw calls are buffered until
    // endFrame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                            const Mat4& view, const Mat4& projection) = 0;
    // Records one draw call for this frame. Each DrawCall supplies its model
    // matrix; the camera comes from beginFrame.
    virtual void submit(const DrawCall& call) = 0;
    // Renders every buffered draw call for the frame.
    virtual void endFrame() = 0;
```

- [ ] **Step 2: Update the `OpenGLRenderer` declarations in `engine/render/backends/opengl/OpenGLRenderer.h`**

Change the `beginFrame` and `submit` override declarations to match:

```cpp
    void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                    const Mat4& view, const Mat4& projection) override;
    void submit(const DrawCall& call) override;
    void endFrame() override;
```

Add these private members alongside the existing ones (next to `DirectionalLight light_;`):

```cpp
    DirectionalLight light_{};
    std::vector<DrawCall> frameCalls_;
    Vec3 clearColor_{};
    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
```

(`<vector>` and `Mat4` are already available in this header.)

- [ ] **Step 3: Rewrite `beginFrame`, `submit`, and `endFrame` in `OpenGLRenderer.cpp`**

Replace the existing `beginFrame`, `submit`, and `endFrame` function bodies with:

```cpp
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

void OpenGLRenderer::endFrame() {
    glClearColor(clearColor_.x, clearColor_.y, clearColor_.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const DrawCall& call : frameCalls_) {
        const GLShader& shader = *shaders_[call.shader - 1];
        shader.bind();
        shader.setMat4("uModel", call.model);
        shader.setMat4("uView", view_);
        shader.setMat4("uProjection", projection_);
        shader.setInt("uTexture", 0);
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

        meshes_[call.mesh - 1]->draw();
    }
}
```

- [ ] **Step 4: Update `RopeTool::draw` to the new `submit` signature**

In `games/02-strandbound/RopeTool.h`, change the `draw` declaration — it no longer needs `view`/`projection`:

```cpp
    // Rebuild and draw the rope tube mesh, and queue an endpoint marker at
    // each rope end as debug lines. Call between beginFrame and endFrame.
    void draw(iron::Renderer& renderer) const;
```

In `games/02-strandbound/RopeTool.cpp`, change the `draw` definition's signature to `void RopeTool::draw(iron::Renderer& renderer) const {` and change the `renderer.submit(...)` call inside it from `renderer.submit(ropeCall, view, projection)` to:

```cpp
    renderer.submit(ropeCall);
```

(The endpoint-marker `renderer.drawLine(...)` calls are unchanged — `drawLine` never took view/projection.)

- [ ] **Step 5: Update `main.cpp` — the render lambda**

In `games/02-strandbound/main.cpp`, the render lambda currently is:

```cpp
    app.setRender([&] {
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light);
        const iron::Mat4 view = (state == PlayerState::Traversing)
                                    ? ropeWalker.viewMatrix()
                                    : player.viewMatrix();
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call, view, projection);
        }

        ropeTool.draw(renderer, view, projection);
        // The thrown rope-end in flight: a small orange cross.
        if (ropeThrower.state() == RopeThrower::State::InFlight) {
            ...
        }
        renderer.flushDebugLines(view, projection);

        hud.setText(...);
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         screenW, screenH);

        renderer.endFrame();
    });
```

Replace it so the camera is computed first, `beginFrame` takes it, `submit`/`draw` lose the camera args, and `endFrame` runs *before* the debug-line and HUD overlays:

```cpp
    app.setRender([&] {
        const iron::Mat4 view = (state == PlayerState::Traversing)
                                    ? ropeWalker.viewMatrix()
                                    : player.viewMatrix();
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light, view,
                            projection);
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call);
        }
        ropeTool.draw(renderer);
        renderer.endFrame();

        // Overlays, drawn on top of the finished lit scene.
        // The thrown rope-end in flight: a small orange cross.
        if (ropeThrower.state() == RopeThrower::State::InFlight) {
            const iron::Vec3 p = ropeThrower.projectilePosition();
            const float s = 0.2f;
            const iron::Vec3 c{1.0f, 0.5f, 0.1f};
            renderer.drawLine(p - iron::Vec3{s, 0.0f, 0.0f},
                              p + iron::Vec3{s, 0.0f, 0.0f}, c);
            renderer.drawLine(p - iron::Vec3{0.0f, s, 0.0f},
                              p + iron::Vec3{0.0f, s, 0.0f}, c);
            renderer.drawLine(p - iron::Vec3{0.0f, 0.0f, s},
                              p + iron::Vec3{0.0f, 0.0f, s}, c);
        }
        renderer.flushDebugLines(view, projection);

        hud.setText(readout,
                    "Ropes: " + std::to_string(ropeTool.ropesAvailable()));
        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         screenW, screenH);
    });
```

(The endpoint-marker debug lines that `RopeTool::draw` queues are flushed by `flushDebugLines` just as before — `drawLine` only enqueues, so queuing them before `endFrame` is fine.)

- [ ] **Step 6: Build and run the test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: clean build; all 12 tests pass.

- [ ] **Step 7: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/02-strandbound/RopeTool.h games/02-strandbound/RopeTool.cpp games/02-strandbound/main.cpp
git commit -m "$(cat <<'EOF'
Buffer the renderer: submit records, endFrame replays

submit now records draw calls instead of drawing immediately;
beginFrame takes the camera; endFrame replays the buffered calls. One
lit pass, no behaviour change — groundwork for multi-pass rendering.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: The shadow pass

Add the depth-from-the-light pass and the shadow term. `endFrame` becomes two passes; the Strandbound lit shader becomes shadow-aware.

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Add `setShadowBounds` to the RHI in `engine/render/Renderer.h`**

In `class Renderer`, after the `endFrame` declaration, add:

```cpp
    // Sets the world-space sphere (centre + radius) the directional light's
    // shadow map must cover. A game calls this once with bounds enclosing its
    // scene.
    virtual void setShadowBounds(Vec3 center, float radius) = 0;
```

- [ ] **Step 2: Update `OpenGLRenderer.h` — include, members, declarations**

Add the include alongside the other backend includes near the top:

```cpp
#include "render/backends/opengl/GLShadowMap.h"
```

Add the `setShadowBounds` override declaration after the `endFrame` override:

```cpp
    void setShadowBounds(Vec3 center, float radius) override;
```

Add a private helper declaration (in the `private:` section, near the other members):

```cpp
    Mat4 computeLightViewProj() const;
```

Add these private members alongside the existing ones (after `GLHud hud_;`):

```cpp
    GLShadowMap shadowMap_;
    GLShader depthShader_;
    Vec3 shadowCenter_{0.0f, 0.0f, 0.0f};
    float shadowRadius_ = 50.0f;
```

- [ ] **Step 3: `OpenGLRenderer.cpp` — includes, the depth shader source, and constants**

Add to the includes at the top of `OpenGLRenderer.cpp`:

```cpp
#include "math/Transform.h"

#include <cmath>
```

Add a file-scope anonymous namespace below the includes (above `namespace iron {`), holding the depth-only shader source and the shadow constants:

```cpp
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

constexpr int kShadowResolution = 2048;
constexpr float kShadowBias = 0.003f;
}  // namespace
```

- [ ] **Step 4: `OpenGLRenderer.cpp` — initialise the new members in the constructor**

The constructor is currently `OpenGLRenderer::OpenGLRenderer() {`. Give it a member initialiser list for the two new GL-resource members (they need a current GL context, which exists by the time the renderer is constructed):

```cpp
OpenGLRenderer::OpenGLRenderer()
    : shadowMap_(kShadowResolution),
      depthShader_(kDepthVertexSrc, kDepthFragmentSrc) {
    glEnable(GL_DEPTH_TEST);
    // ... the rest of the existing constructor body is unchanged ...
```

(Leave the existing constructor body — the fallback and white textures — exactly as it is.)

- [ ] **Step 5: `OpenGLRenderer.cpp` — add `computeLightViewProj` and `setShadowBounds`**

Add these two functions (anywhere among the member definitions, e.g. just before `endFrame`):

```cpp
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
                                   0.1f, shadowRadius_ * 4.0f);
    return proj * view;
}
```

- [ ] **Step 6: `OpenGLRenderer.cpp` — rewrite `endFrame` as two passes**

Replace the entire `endFrame` function (the single-lit-pass version from Task 3) with:

```cpp
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
}
```

- [ ] **Step 7: `main.cpp` — make the lit shader shadow-aware**

In `games/02-strandbound/main.cpp`, replace the `kVertexShader` string constant with:

```cpp
// Vertex shader: MVP transform; passes world-space normal, UV, and the
// light-space position (for the shadow lookup) through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightViewProj;

out vec3 vNormal;
out vec2 vUV;
out vec4 vLightSpacePos;

void main() {
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vLightSpacePos = uLightViewProj * uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";
```

And replace the `kFragmentShader` string constant with:

```cpp
// Fragment shader: Lambert diffuse from one directional light + ambient, with
// the diffuse term darkened where the fragment is in shadow.
const char* kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vLightSpacePos;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2D uShadowMap;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;
uniform float uShadowBias;

// 1.0 = lit, 0.0 = in shadow.
float shadowFactor() {
    vec3 proj = vLightSpacePos.xyz / vLightSpacePos.w;
    proj = proj * 0.5 + 0.5;  // [-1,1] -> [0,1]
    if (proj.z > 1.0) {
        return 1.0;  // beyond the shadow map's far plane: lit
    }
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;  // outside the shadow map: lit
    }
    float stored = texture(uShadowMap, proj.xy).r;
    return (proj.z - uShadowBias > stored) ? 0.0 : 1.0;
}

void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    float shadow = shadowFactor();
    vec3 lighting = uLightColor * (diffuse * shadow + uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
}
)";
```

- [ ] **Step 8: `main.cpp` — set the shadow bounds**

In `main()`, after the `RopeTool ropeTool(renderer, shader);` line (and the `RopeThrower ropeThrower;` line), add a call setting the shadow bounds to cover the Strandbound level (the home island, the gap, and the far island):

```cpp
    // The shadow map must cover the whole level — both islands and the gap.
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, -22.0f}, 45.0f);
```

- [ ] **Step 9: Build and run the test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: clean build; all 12 tests pass.

- [ ] **Step 10: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/02-strandbound/main.cpp
git commit -m "$(cat <<'EOF'
Add directional-light shadow mapping

endFrame now runs a depth pass from the light into GLShadowMap, then
the lit pass samples it — the islands, props, pole, and ropes cast
hard shadows. The Strandbound lit shader gains the shadow term.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Concept note

**Files:**
- Create: `docs/engine/shadow-mapping.md`

- [ ] **Step 1: Create `docs/engine/shadow-mapping.md`**

```markdown
# Shadow mapping

The engine renders real-time shadows for the directional light. It is the
engine's first multi-pass render feature and its first use of
render-to-texture.

## The buffered renderer

Shadow mapping needs two passes per frame, so the renderer is buffered rather
than immediate-mode:

- `beginFrame` records the clear colour, the light, and the camera.
- `submit` records a draw call — it does not draw.
- `endFrame` replays the buffered calls in two passes.

Debug lines and the HUD are overlays: the game draws them after `endFrame`.

## The two passes

1. **Shadow pass.** The scene's depth is rendered from the sun's viewpoint into
   `GLShadowMap` — a framebuffer with a single depth-texture attachment. The
   light is directional, so its "camera" is an orthographic box
   (`orthographic()`) aimed along the light direction, sized to a settable
   scene-bounds sphere (`setShadowBounds`).
2. **Lit pass.** The scene is rendered normally to the screen. The lit shader
   transforms each fragment into the light's space, samples the shadow map's
   stored depth, and darkens the diffuse term where the fragment is farther
   from the light than the stored depth (plus a small bias to avoid acne).
   Ambient is unaffected, so shadowed surfaces are dim, not black.

## Limitations / future work

Hard shadows only (one depth sample per fragment) — PCF soft edges are a
follow-up. One directional light. A single shadow map with a fixed
scene-covering frustum — no cascades. Point-light and spotlight shadows, and
reflections (which reuse this render-to-texture machinery), are future
milestones.
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/shadow-mapping.md
git commit -m "$(cat <<'EOF'
Add shadow-mapping concept note

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Done

After Task 5, the engine renders directional-light shadows: a buffered, two-pass renderer, a depth framebuffer, and a shadow-aware lit shader. Hand off to `superpowers:finishing-a-development-branch`.
