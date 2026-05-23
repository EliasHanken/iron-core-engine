# Atmosphere Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add exponential distance fog, a cubemap skybox, and horizon fog
blending to the engine; Strandbound becomes a sunset scene with warm orange
fog and a painted sunset cubemap.

**Architecture:** Three additive pieces. `Fog { color, density }` rides
with `Renderer::beginFrame` like the directional light and point lights do.
A `GLCubemap` wraps a `GL_TEXTURE_CUBE_MAP`; a new `GLSkybox` pass draws a
unit cube at the camera position between the lit pass and the HUD pass.
Fog blend is in the lit fragment shader; horizon fog blend is in the
skybox fragment shader.

**Tech Stack:** C++23, OpenGL 3.3 (existing RHI), GLSL, stb_image.

**Spec:** `docs/superpowers/specs/2026-05-23-atmosphere-design.md`.

**Adaptation from spec — cubemap asset source.** The spec named a
CC0 PNG cubemap from Polyhaven as the skybox content source. This plan
substitutes a **procedurally-generated sunset cubemap built at startup**
because I cannot reliably download CC0 PNG cubemap files inside this
session. The engine machinery is identical: real `GL_TEXTURE_CUBE_MAP`,
real `samplerCube` sampling, real skybox pass. Only the *content* shifts
from "PNG files on disk" to "RGBA pixel arrays generated in C++". A small
follow-up milestone (≈30 minutes) can add `loadCubemap(6 paths)` once the
user has CC0 assets to point at. The RHI verb in this plan is
`createCubemap(width, height, std::array<const unsigned char*, 6>)`,
mirroring the existing `createTexture` (in-memory) vs `loadTexture` (disk)
pair.

**Task order rationale:** Each task leaves a green build. Tasks 1–4 are
additive (no behaviour change). Task 5 is the atomic `beginFrame`
signature change, with both games still rendering identically. Task 6
adds the skybox pass machinery — no-op until a game calls `setSkybox`.
Task 7 lights up Strandbound. Task 8 docs.

---

## Task 1: Fog struct + CubemapHandle typedef + Scene::fog field

**Files:**
- Create: `engine/render/Fog.h`
- Modify: `engine/render/Handles.h`
- Modify: `engine/scene/Scene.h`

Purely additive — no caller change. Defaults (density 0, empty handle)
keep existing demos visually identical.

- [ ] **Step 1: Create the Fog struct**

Create `engine/render/Fog.h`:

```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// Distance fog: every lit fragment blends toward `color` with weight
// `1 - exp(-density * distFromCamera)`. The skybox pass also blends
// toward this colour near the horizon to dissolve the sky/terrain edge.
//
// `density = 0` (the default) disables fog entirely — existing demos
// pass a default-constructed Fog and see no visual change.
struct Fog {
    Vec3 color{0.7f, 0.6f, 0.5f}; // warm-grey by default
    float density = 0.0f;          // 0 = no fog
};

} // namespace iron
```

- [ ] **Step 2: Add CubemapHandle typedef to Handles.h**

Open `engine/render/Handles.h`. Below the existing `TextureHandle` line,
add:

```cpp
// Cubemap textures (skyboxes, environment maps). 0 = invalid.
using CubemapHandle = std::uint32_t;
```

(If `Handles.h` doesn't already include `<cstdint>`, it does — verify
before adding it again. The existing `MeshHandle`/`TextureHandle` are
already typed `std::uint32_t`.)

- [ ] **Step 3: Add Fog field to Scene**

Open `engine/scene/Scene.h`. At the top, add `#include "render/Fog.h"`.
Update the `Scene` struct to add a `Fog fog;` field:

```cpp
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
    std::vector<PointLight> pointLights;
    Fog fog;                       // NEW
};
```

Update the struct's doc comment to mention "plus optional fog" or
similar.

- [ ] **Step 4: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 13/13 tests pass (no test changes, no behaviour
change).

- [ ] **Step 5: Commit**

```powershell
git add engine/render/Fog.h engine/render/Handles.h engine/scene/Scene.h
git commit -m @'
Add Fog struct, CubemapHandle, Scene::fog field

Additive ahead of the renderer-surface change. Defaults are
color = warm-grey, density = 0 (fog disabled), so all existing
code keeps current behaviour; the renderer ignores these fields
until beginFrame is wired to them in a later commit.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 2: GLCubemap class

**Files:**
- Create: `engine/render/backends/opengl/GLCubemap.h`
- Create: `engine/render/backends/opengl/GLCubemap.cpp`

Wraps a `GL_TEXTURE_CUBE_MAP`. Constructed from in-memory RGBA face
data. Six faces are uploaded with `glTexImage2D` to the six cube-map
targets. Pure addition — no caller wires it up yet.

- [ ] **Step 1: Create the header**

Create `engine/render/backends/opengl/GLCubemap.h`:

```cpp
#pragma once

#include <array>
#include <cstdint>

namespace iron {

// Wraps a GL_TEXTURE_CUBE_MAP. Six RGBA8 face textures (all the same
// size); GL_CLAMP_TO_EDGE on S/T/R; linear min/mag filtering. Face
// order matches OpenGL: +X, -X, +Y, -Y, +Z, -Z.
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
```

- [ ] **Step 2: Create the implementation**

Create `engine/render/backends/opengl/GLCubemap.cpp`:

```cpp
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
```

- [ ] **Step 3: Add to engine CMakeLists if needed**

The engine library likely picks up `engine/render/backends/opengl/*.cpp`
via glob — verify by checking `engine/CMakeLists.txt` for a `file(GLOB
...)` pattern that includes opengl backend sources. If sources are
listed explicitly, add `engine/render/backends/opengl/GLCubemap.cpp` to
the appropriate `add_library(ironcore ...)` target.

- [ ] **Step 4: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build. (No tests can exercise GLCubemap headlessly —
construction requires a GL context.)

- [ ] **Step 5: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 13/13 pass.

- [ ] **Step 6: Commit**

```powershell
git add engine/render/backends/opengl/GLCubemap.h engine/render/backends/opengl/GLCubemap.cpp
git commit -m @'
Add GLCubemap class

Wraps GL_TEXTURE_CUBE_MAP with six RGBA8 face uploads from
in-memory pixel data. Linear filtering, clamp-to-edge.
Validates face pointers (null → invalid cubemap, logs error).
No caller wires it up yet; that lands when the renderer
exposes createCubemap + setSkybox.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 3: Renderer::createCubemap + setSkybox

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`

Adds two RHI verbs. `createCubemap` produces a `CubemapHandle` from in-
memory RGBA face data. `setSkybox` records which cubemap is the active
sky for subsequent frames. No rendering yet — the skybox pass lands in
Task 6.

- [ ] **Step 1: Add the virtuals to the Renderer interface**

Open `engine/render/Renderer.h`. Add `#include <array>` near the top
(alongside `<string>`, `<span>`). Add `#include "render/Handles.h"` if
not already present. After the existing texture / shader creation
methods, add:

```cpp
// Creates a cubemap texture from six RGBA face arrays. Each face is
// `width * height * 4` bytes. Face order: +X, -X, +Y, -Y, +Z, -Z.
// Returns kInvalidHandle if any face is null or dimensions invalid.
virtual CubemapHandle createCubemap(
    int width, int height,
    std::array<const unsigned char*, 6> faces) = 0;

// Registers a cubemap as the skybox for subsequent frames. Pass
// kInvalidHandle to disable the skybox.
virtual void setSkybox(CubemapHandle sky) = 0;
```

- [ ] **Step 2: Add the override declarations to OpenGLRenderer**

Open `engine/render/backends/opengl/OpenGLRenderer.h`. Add the override
declarations alongside the other resource-creation methods:

```cpp
CubemapHandle createCubemap(
    int width, int height,
    std::array<const unsigned char*, 6> faces) override;
void setSkybox(CubemapHandle sky) override;
```

Add `#include <array>` if not already present. Add private members near
the other resource vectors:

```cpp
std::vector<std::unique_ptr<GLCubemap>> cubemaps_;
CubemapHandle skybox_ = kInvalidHandle;
```

Add `#include "render/backends/opengl/GLCubemap.h"` near the other GL
includes.

- [ ] **Step 3: Implement createCubemap**

Open `engine/render/backends/opengl/OpenGLRenderer.cpp`. Add the
implementation alongside the other resource creators:

```cpp
CubemapHandle OpenGLRenderer::createCubemap(
    int width, int height,
    std::array<const unsigned char*, 6> faces) {
    auto cubemap = std::make_unique<GLCubemap>(width, height, faces);
    if (!cubemap->isValid()) {
        return kInvalidHandle;
    }
    cubemaps_.push_back(std::move(cubemap));
    return static_cast<CubemapHandle>(cubemaps_.size());
}

void OpenGLRenderer::setSkybox(CubemapHandle sky) {
    skybox_ = sky;
}
```

(Handle convention is vector-index + 1, matching meshes/textures.)

- [ ] **Step 4: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 5: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 13/13.

- [ ] **Step 6: Commit**

```powershell
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp
git commit -m @'
Add Renderer::createCubemap and setSkybox

createCubemap takes six RGBA face arrays (in-memory, like the
existing createTexture path) and returns a CubemapHandle.
setSkybox records the active sky for the renderer to draw.
Neither verb has a caller yet — the skybox pass lands in a
later commit.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 4: beginFrame takes Fog; fog uniforms uploaded in lit pass

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/02-strandbound/main.cpp`

Atomic interface change. `beginFrame` grows a `const Fog& fog`
parameter; the OpenGL backend stores it and uploads `uFogColor` /
`uFogDensity` in the lit pass alongside the existing sun/shadow/point-
light uniforms. Both games pass a default `Fog{}` (density 0) — visually
unchanged.

The spinning-cube fragment shader also declares the two new uniforms (so
the upload targets a real location and isn't silently dropped). The
cube's shader body stays unlit textured — visually identical.

- [ ] **Step 1: Update the Renderer interface**

Open `engine/render/Renderer.h`. Add `#include "render/Fog.h"` to the
includes (it transitively gets `Vec.h`). Update the `beginFrame`
signature to insert `const Fog& fog` between `pointLights` and `view`:

```cpp
virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                        std::span<const PointLight> pointLights,
                        const Fog& fog,
                        const Mat4& view, const Mat4& projection) = 0;
```

- [ ] **Step 2: Update OpenGLRenderer header**

Open `engine/render/backends/opengl/OpenGLRenderer.h`. Update the
override signature to match. Add a private member to store the fog:

```cpp
Fog fog_{};
```

Place it near `light_` / `pointLights_` (other per-frame state).

- [ ] **Step 3: Update OpenGLRenderer.cpp beginFrame body**

Open `engine/render/backends/opengl/OpenGLRenderer.cpp`. Update the
`beginFrame` implementation:

```cpp
void OpenGLRenderer::beginFrame(Vec3 clearColor,
                                const DirectionalLight& light,
                                std::span<const PointLight> pointLights,
                                const Fog& fog,
                                const Mat4& view,
                                const Mat4& projection) {
    clearColor_ = clearColor;
    light_ = light;
    fog_ = fog;
    view_ = view;
    projection_ = projection;
    frameCalls_.clear();

    pointLights_.clear();
    if (pointLights.size() > static_cast<std::size_t>(kMaxPointLights)) {
        Log::warn("OpenGLRenderer: %zu point lights submitted, capping at %d",
                  pointLights.size(), kMaxPointLights);
        pointLights_.assign(pointLights.begin(),
                            pointLights.begin() + kMaxPointLights);
    } else {
        pointLights_.assign(pointLights.begin(), pointLights.end());
    }
}
```

(Only the `fog_ = fog;` line is new; the rest is unchanged from Task 4
of the prior milestone.)

- [ ] **Step 4: Upload fog uniforms in the lit pass**

In `OpenGLRenderer.cpp`'s `endFrame`, inside the lit-pass per-draw loop,
**after** the existing point-light upload block and **before** (or just
after) the `setVec3("uEmissive", call.emissive)` line, add:

```cpp
shader.setVec3("uFogColor", fog_.color);
shader.setFloat("uFogDensity", fog_.density);
```

These are per-frame state but uploaded per draw (mirroring the existing
sun-uniform pattern). A future renderer milestone can hoist them to
once-per-shader-bind.

- [ ] **Step 5: Update game callers — 01-spinning-cube**

Open `games/01-spinning-cube/main.cpp`. Add `#include "render/Fog.h"` if
not already present (it's transitively included via `Renderer.h`, but
an explicit include makes the dependency clear).

Find the `renderer.beginFrame(...)` call and add `iron::Fog{}` (a
default fog) between the empty point-light span and the view:

```cpp
renderer.beginFrame(clearColor, sun,
                    std::span<const iron::PointLight>{},
                    iron::Fog{},                       // NEW — default = no fog
                    view, projection);
```

Also update the spinning-cube **fragment shader** to declare the new
uniforms (so uploads aren't silent no-ops). The shader body stays the
same:

```glsl
// In the fragment shader source string near the existing uniform decls:
uniform vec3 uFogColor;
uniform float uFogDensity;
// (no use in main — declared so the renderer's upload finds locations)
```

Update the comment above the fragment shader to mention "fog uniforms
declared but unused; cube stays unlit textured."

- [ ] **Step 6: Update game callers — 02-strandbound**

Open `games/02-strandbound/main.cpp`. Find the `renderer.beginFrame(...)`
call (Task 4 of the previous milestone replaced the placeholder span
with `scene.pointLights`). Add `scene.fog` between the point lights and
the view:

```cpp
renderer.beginFrame(clearColor, scene.light,
                    std::span<const iron::PointLight>(scene.pointLights),
                    scene.fog,                          // NEW
                    view, projection);
```

`scene.fog` is default-constructed (density 0) at this point — visually
identical. Task 7 will populate it.

DO NOT modify Strandbound's shader source in this task. Task 7 does
that.

- [ ] **Step 7: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 8: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 13/13.

- [ ] **Step 9: Optional — launch both games and confirm no visual change**

If GUI launch is possible: `01-spinning-cube` is still an unlit textured
cube on the clear-color background. `02-strandbound` still has its
lights and shadows. Neither shows fog yet.

Skip this step if you cannot launch GUI apps. The user playtests at the
end.

- [ ] **Step 10: Commit**

```powershell
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp games/02-strandbound/main.cpp
git commit -m @'
Renderer::beginFrame takes a Fog parameter

Adds Fog (color + density) to the per-frame data the renderer
records. The lit pass uploads uFogColor and uFogDensity per
shader bind. Both games pass default Fog{} (density 0); the
spinning-cube fragment shader declares the new uniforms so
the upload targets real locations. No visual change yet — fog
density 0 means the blend is a no-op.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 5: GLSkybox class + skybox pass in endFrame

**Files:**
- Create: `engine/render/backends/opengl/GLSkybox.h`
- Create: `engine/render/backends/opengl/GLSkybox.cpp`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`

Adds the skybox rendering machinery. `GLSkybox` owns a unit cube VBO,
the skybox `GLShader`, and exposes a `draw(view, projection, cubemap,
fogColor, horizonBand)` method. The OpenGLRenderer instantiates one
`GLSkybox` in its constructor and calls `draw` in `endFrame` after the
lit pass and before debug lines / HUD, but only when `skybox_` is a
valid handle.

**State management:** the skybox pass runs with `GL_DEPTH_FUNC = LEQUAL`
and depth-write off; restores `GL_DEPTH_FUNC = LESS` and depth-write on
afterward.

- [ ] **Step 1: Create the header**

Create `engine/render/backends/opengl/GLSkybox.h`:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/backends/opengl/GLCubemap.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>

namespace iron {

// Renders a cubemap skybox. Owns the unit cube VBO/VAO and the skybox
// shader. The cube is drawn at the camera position (the view matrix's
// translation is stripped), forced to gl_FragDepth = 1.0 so all
// geometry draws on top.
class GLSkybox {
public:
    GLSkybox();
    ~GLSkybox();

    GLSkybox(const GLSkybox&) = delete;
    GLSkybox& operator=(const GLSkybox&) = delete;

    bool isValid() const { return vao_ != 0 && shader_.isValid(); }

    // Draws the skybox. Caller must have a depth buffer cleared to 1.0
    // (or geometry already rendered such that empty pixels are at
    // depth 1.0). State is saved and restored.
    void draw(const Mat4& view, const Mat4& projection,
              const GLCubemap& sky, Vec3 fogColor, float horizonBand) const;

private:
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
    GLShader shader_;
};

} // namespace iron
```

- [ ] **Step 2: Create the implementation**

Create `engine/render/backends/opengl/GLSkybox.cpp`:

```cpp
#include "render/backends/opengl/GLSkybox.h"

#include "core/Log.h"

#include <glad/gl.h>

namespace iron {

namespace {

// 36-vertex unit cube (6 faces * 2 triangles * 3 verts). Vertex
// positions are used as world-space directions in the skybox shader,
// so the cube must span ±1.
const float kCubeVertices[] = {
    // +X
     1, -1, -1,  1,  1, -1,  1,  1,  1,
     1, -1, -1,  1,  1,  1,  1, -1,  1,
    // -X
    -1, -1,  1, -1,  1,  1, -1,  1, -1,
    -1, -1,  1, -1,  1, -1, -1, -1, -1,
    // +Y
    -1,  1, -1, -1,  1,  1,  1,  1,  1,
    -1,  1, -1,  1,  1,  1,  1,  1, -1,
    // -Y
    -1, -1,  1, -1, -1, -1,  1, -1, -1,
    -1, -1,  1,  1, -1, -1,  1, -1,  1,
    // +Z
    -1, -1,  1,  1, -1,  1,  1,  1,  1,
    -1, -1,  1,  1,  1,  1, -1,  1,  1,
    // -Z
     1, -1, -1, -1, -1, -1, -1,  1, -1,
     1, -1, -1, -1,  1, -1,  1,  1, -1,
};

const char* kSkyboxVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vWorldDir;
void main() {
    vWorldDir = aPos;
    // Strip translation from view so the sky doesn't move with the camera.
    mat4 viewNoTranslation = mat4(mat3(uView));
    vec4 clip = uProjection * viewNoTranslation * vec4(aPos, 1.0);
    // Force gl_FragDepth = 1.0 (far plane) so geometry draws on top.
    gl_Position = clip.xyww;
}
)";

const char* kSkyboxFragmentShader = R"(#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;
uniform samplerCube uSkyCubemap;
uniform vec3 uFogColor;
uniform float uHorizonFogBand;
void main() {
    vec3 dir = normalize(vWorldDir);
    vec3 skyColor = texture(uSkyCubemap, dir).rgb;
    // Blend with fog colour near the horizon. abs(dir.y) is 0 at the
    // horizon and 1 at zenith/nadir. smoothstep ramps smoothly.
    float horizonMix = smoothstep(0.0, uHorizonFogBand, abs(dir.y));
    vec3 result = mix(uFogColor, skyColor, horizonMix);
    FragColor = vec4(result, 1.0);
}
)";

} // namespace

GLSkybox::GLSkybox()
    : shader_(kSkyboxVertexShader, kSkyboxFragmentShader) {
    if (!shader_.isValid()) {
        Log::error("GLSkybox: shader failed to compile");
        return;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

GLSkybox::~GLSkybox() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLSkybox::draw(const Mat4& view, const Mat4& projection,
                    const GLCubemap& sky, Vec3 fogColor,
                    float horizonBand) const {
    if (!isValid() || !sky.isValid()) return;

    // Save state we modify.
    GLboolean savedDepthMask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedDepthFunc;
    glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    shader_.bind();
    shader_.setMat4("uView", view);
    shader_.setMat4("uProjection", projection);
    shader_.setInt("uSkyCubemap", 0);
    shader_.setVec3("uFogColor", fogColor);
    shader_.setFloat("uHorizonFogBand", horizonBand);

    sky.bind(0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    // Restore state.
    glDepthFunc(savedDepthFunc);
    glDepthMask(savedDepthMask);
}

} // namespace iron
```

- [ ] **Step 3: Add GLSkybox member to OpenGLRenderer**

Open `engine/render/backends/opengl/OpenGLRenderer.h`. Add the include:

```cpp
#include "render/backends/opengl/GLSkybox.h"
```

Add a private member alongside the other GL helpers:

```cpp
GLSkybox skybox_pass_;
```

Place it after `hud_` or near the other helper objects.

Also add a constant near the top of `OpenGLRenderer.cpp` (in the
anonymous namespace if one exists):

```cpp
constexpr float kHorizonFogBand = 0.25f;
```

- [ ] **Step 4: Call the skybox pass in endFrame**

In `OpenGLRenderer::endFrame`, after the lit-pass loop closes and after
the texture-unit-1 cleanup (the existing `glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE0);` block),
but **before** the debug-lines / HUD calls (those happen later or via
`drawHud`), add:

```cpp
// --- Pass 3: skybox (only if one is registered) ---
if (skybox_ != kInvalidHandle && skybox_ <= cubemaps_.size()) {
    skybox_pass_.draw(view_, projection_, *cubemaps_[skybox_ - 1],
                      fog_.color, kHorizonFogBand);
}
```

Read the existing `endFrame` carefully before inserting — the exact
placement is between the lit pass cleanup and the function's end. The
debug-lines and HUD passes happen via separate `flushDebugLines` /
`drawHud` calls that the game invokes after `endFrame`, so the skybox
ends up before those naturally (they're called by the game, not by the
renderer).

- [ ] **Step 5: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 6: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 13/13.

- [ ] **Step 7: Optional — launch games**

Neither game has called `setSkybox` yet, so visuals are unchanged. The
skybox pass is a no-op because `skybox_ == kInvalidHandle`. Skip if you
can't launch GUI apps.

- [ ] **Step 8: Commit**

```powershell
git add engine/render/backends/opengl/GLSkybox.h engine/render/backends/opengl/GLSkybox.cpp engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp
git commit -m @'
Add GLSkybox class and skybox render pass

GLSkybox owns the unit cube VBO and the skybox shader; its
draw method renders the cubemap at the camera position with
the horizon fog blend. OpenGLRenderer instantiates one and
calls draw after the lit pass if a cubemap has been registered
via setSkybox. The pass is a no-op until a game calls
setSkybox with a valid handle.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 6: Light up Strandbound with sunset atmosphere

**File:** `games/02-strandbound/main.cpp` (game-only changes).

The user-visible payoff. Adds:
- A procedural sunset cubemap generator (helper function in main.cpp).
- Call `renderer.createCubemap(...)` at startup with the generated face
  data; call `renderer.setSkybox(...)` with the returned handle.
- Vertex shader: add `out vec3 vViewPos;` and `vViewPos = (uView *
  uModel * vec4(aPos, 1.0)).xyz;`
- Fragment shader: add `uFogColor`, `uFogDensity` uniforms, `in vec3
  vViewPos;`, restructure the final colour composition to apply the fog
  `mix` at the end.
- Set `scene.fog = Fog{Vec3{0.85f, 0.55f, 0.4f}, 0.025f}` (warm sunset
  orange).
- No `beginFrame` call change needed — Task 4 already passes `scene.fog`.

- [ ] **Step 1: Update the vertex shader to output vViewPos**

Open `games/02-strandbound/main.cpp`. Find `kVertexShader`. Add
`out vec3 vViewPos;` to the outputs and compute it in `main()`. The
existing `worldPos4` line is reused; we also need the view-space
position. Final form (additions clearly marked):

```glsl
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vLightSpacePos;
out vec3 vViewPos;                         // NEW

void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vLightSpacePos = uLightViewProj * worldPos4;
    vec4 viewPos4 = uView * worldPos4;     // NEW (reuse worldPos4)
    vViewPos = viewPos4.xyz;               // NEW
    gl_Position = uProjection * viewPos4;  // reuse viewPos4
}
```

- [ ] **Step 2: Update the fragment shader to apply fog**

Find `kFragmentShader`. Add the new uniform declarations and input, and
restructure the final colour composition. Current state ends with:

```glsl
vec4 texel = texture(uTexture, vUV);
FragColor = vec4(texel.rgb * lighting + uEmissive, texel.a);
```

Replace with the new fog-aware form. Full set of changes (additions
clearly marked):

```glsl
// Add to the in/uniform block near the top of the fragment shader:
in vec3 vViewPos;                          // NEW

uniform vec3 uFogColor;                    // NEW
uniform float uFogDensity;                 // NEW

// In main(), replace the final composition (last 2 lines) with:
vec4 texel = texture(uTexture, vUV);
vec3 litColor = texel.rgb * lighting + uEmissive;
// Distance fog. View-space length equals world-space distance because
// the view matrix doesn't scale.
float distFromCamera = length(vViewPos);
float fogFactor = 1.0 - exp(-uFogDensity * distFromCamera);
vec3 finalColor = mix(litColor, uFogColor, fogFactor);
FragColor = vec4(finalColor, texel.a);
```

(All the sun + shadow + point-light + emissive math above this stays
exactly as it is.)

- [ ] **Step 3: Add the procedural sunset cubemap generator**

In `games/02-strandbound/main.cpp`, in the anonymous namespace (or near
the top, before `WinMain` / `main`), add this helper function:

```cpp
namespace {

constexpr int kSkyFaceSize = 256;

// Generates one face of the sunset cubemap. `face` is the OpenGL face
// index: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z. Writes
// kSkyFaceSize*kSkyFaceSize*4 RGBA bytes into `pixels`.
//
// Sunset palette: deep blue at zenith, warm magenta in the middle,
// glowing orange at the horizon. Side faces get a vertical gradient;
// the top is pure zenith blue; the bottom is dark warm ground.
void generateSunsetFace(int face, std::vector<unsigned char>& pixels) {
    pixels.assign(kSkyFaceSize * kSkyFaceSize * 4, 0);

    const iron::Vec3 cZenith{0.10f, 0.18f, 0.40f};   // deep blue
    const iron::Vec3 cMid   {0.85f, 0.45f, 0.45f};   // warm magenta
    const iron::Vec3 cHoriz {1.00f, 0.55f, 0.30f};   // glowing orange
    const iron::Vec3 cGround{0.20f, 0.12f, 0.10f};   // dark warm

    for (int y = 0; y < kSkyFaceSize; ++y) {
        for (int x = 0; x < kSkyFaceSize; ++x) {
            // Convert (x, y) into a direction on this face. Each face
            // is the unit cube face: u, v in [-1, 1].
            const float u = 2.0f * (x + 0.5f) / kSkyFaceSize - 1.0f;
            const float v = 2.0f * (y + 0.5f) / kSkyFaceSize - 1.0f;
            iron::Vec3 dir;
            switch (face) {
                case 0: dir = { 1.0f, -v,   -u};   break;  // +X
                case 1: dir = {-1.0f, -v,    u};   break;  // -X
                case 2: dir = { u,    1.0f,  v};   break;  // +Y
                case 3: dir = { u,   -1.0f, -v};   break;  // -Y
                case 4: dir = { u,   -v,    1.0f}; break;  // +Z
                case 5: dir = {-u,   -v,   -1.0f}; break;  // -Z
            }
            const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            dir = {dir.x/len, dir.y/len, dir.z/len};

            // Vertical position in the sky: dir.y in [-1, 1].
            const float skyY = dir.y;
            iron::Vec3 color;
            if (skyY >= 0.0f) {
                // Above the horizon: blend mid → zenith as y goes 0 → 1.
                const float t = skyY;            // 0 at horizon, 1 at zenith
                const float horizMid = std::min(t * 2.0f, 1.0f);  // horizon→mid in first half
                const float midZen   = std::max((t - 0.5f) * 2.0f, 0.0f); // mid→zenith in second half
                iron::Vec3 a = {
                    cHoriz.x + (cMid.x - cHoriz.x) * horizMid,
                    cHoriz.y + (cMid.y - cHoriz.y) * horizMid,
                    cHoriz.z + (cMid.z - cHoriz.z) * horizMid,
                };
                color = {
                    a.x + (cZenith.x - a.x) * midZen,
                    a.y + (cZenith.y - a.y) * midZen,
                    a.z + (cZenith.z - a.z) * midZen,
                };
            } else {
                // Below the horizon: blend horizon → ground as y goes 0 → -1.
                const float t = -skyY;           // 0 at horizon, 1 at nadir
                color = {
                    cHoriz.x + (cGround.x - cHoriz.x) * t,
                    cHoriz.y + (cGround.y - cHoriz.y) * t,
                    cHoriz.z + (cGround.z - cHoriz.z) * t,
                };
            }

            const int idx = (y * kSkyFaceSize + x) * 4;
            pixels[idx + 0] = static_cast<unsigned char>(
                std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<unsigned char>(
                std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<unsigned char>(
                std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
}

} // namespace
```

Add includes if not present: `<algorithm>` (for `std::clamp`), `<cmath>`
(for `std::sqrt`), `<vector>` (likely already there).

- [ ] **Step 4: Build the cubemap at startup and call setSkybox**

In `main` (or wherever the scene is initialised — early, before the
render loop), after the `OpenGLRenderer renderer;` line:

```cpp
// Build the sunset cubemap procedurally (no external assets needed
// for this milestone; loadCubemap from disk is a follow-up task).
std::vector<unsigned char> faceData[6];
std::array<const unsigned char*, 6> facePtrs{};
for (int i = 0; i < 6; ++i) {
    generateSunsetFace(i, faceData[i]);
    facePtrs[i] = faceData[i].data();
}
iron::CubemapHandle skybox = renderer.createCubemap(
    kSkyFaceSize, kSkyFaceSize, facePtrs);
renderer.setSkybox(skybox);
if (skybox == iron::kInvalidHandle) {
    iron::Log::warn("Sunset cubemap creation failed; sky will be"
                    " the clear colour");
}
```

(The `faceData` vectors outlive the `createCubemap` call, so the
pointers remain valid for the duration of the upload. After upload, GL
owns the texture and the local vectors can go out of scope.)

Add `#include <array>` to the top of `main.cpp` if not already there.

- [ ] **Step 5: Set scene.fog to sunset orange**

Find where `scene.light` is initialised (or where the sun ambient is set
to 0.15 from the previous milestone). Below it, add:

```cpp
scene.fog.color = iron::Vec3{0.85f, 0.55f, 0.4f};
scene.fog.density = 0.025f;
```

Numbers: warm orange that complements the sunset cubemap's horizon;
density 0.025 → at 50 world units (home → far island distance) fog
weight is `1 - exp(-1.25) ≈ 0.71`. Tunable.

- [ ] **Step 6: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 7: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 13/13.

- [ ] **Step 8: Optional — launch Strandbound**

You should see a sunset sky around the level, fog tinting the far
island, and the bridge anchor pole looking visibly more distant. If
launching is not possible, leave the playtest checklist for the user.

- [ ] **Step 9: Commit**

```powershell
git add games/02-strandbound/main.cpp
git commit -m @'
Light up Strandbound with sunset atmosphere

Adds a procedural sunset cubemap generator (zenith blue,
warm-magenta mid, glowing orange horizon, dark warm ground),
builds the cubemap at startup, registers it as the skybox.
Sets scene.fog to warm orange (density 0.025 ~ 71% fog at
50 units). Vertex shader gains vViewPos; fragment shader
applies an exp(-density * dist) fog mix at the end. Texture
alpha preserved.

Only game code changes; no engine change in this commit.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 7: Update lighting/atmosphere docs

**File:** `docs/engine/lighting.md`

Add a short section on atmosphere (fog + skybox) — matches the file's
tone (narrative concept paragraphs, not code walkthroughs).

- [ ] **Step 1: Read the existing doc**

```powershell
Get-Content docs/engine/lighting.md
```

(In practice use the `Read` tool.)

- [ ] **Step 2: Append an Atmosphere section**

Add a new section before the "Related:" footer, after the "Point lights"
section:

```markdown
## Atmosphere

The engine adds two atmospheric pieces on top of the lighting model.

**Distance fog** blends each lit fragment toward a fog colour with
weight `1 - exp(-density * distance)`. Picking `density` is how you
author fog reach: small values mean far visibility, larger values mean
the world fades into mist. This makes scale and distance *legible* —
the bridge gap reads as far away because the far island is fog-tinted,
not because it's small on screen.

**Cubemap skybox** is a real `GL_TEXTURE_CUBE_MAP` sampled in a
fragment shader that renders a unit cube at the camera position. The
view matrix's translation is stripped so the sky never moves with the
player; only rotation affects what's seen. The skybox is drawn at
`gl_FragDepth = 1.0` (the far plane) so all geometry renders on top of
it. The sun, clouds, and any other sky detail are part of the painted
cubemap.

**Horizon fog blend.** Both pieces meet at the horizon: the skybox
fragment shader also blends toward the fog colour where the view
direction's vertical component is small (`abs(dir.y)` close to zero).
This dissolves the otherwise-sharp edge where geometry meets sky into a
soft band — the trick that makes a fogged scene look "atmospheric"
rather than "bolted on."

Together, fog density, fog colour, and the cubemap's horizon tint
should be authored as a set so they coordinate. A future milestone can
add a screen-space fog post-pass (useful once bloom or tonemapping
exists) and image-based lighting from the cubemap.
```

- [ ] **Step 3: Commit**

```powershell
git add docs/engine/lighting.md
git commit -m @'
Document atmosphere (fog + cubemap skybox + horizon blend)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Self-review checklist (the planner's)

After implementation, the planner verifies:

- [ ] Spec coverage:
  - `Fog` struct + defaults → Task 1 ✓
  - `CubemapHandle` typedef → Task 1 ✓
  - `Scene::fog` field → Task 1 ✓
  - `GLCubemap` class → Task 2 ✓
  - `Renderer::createCubemap` (in-memory adaptation of spec's
    `loadCubemap`) → Task 3 ✓
  - `Renderer::setSkybox` → Task 3 ✓
  - `beginFrame` takes `const Fog& fog` → Task 4 ✓
  - Lit shader fog uniforms + blend → Task 4 (uniform upload) + Task 6
    (Strandbound shader body) ✓
  - Spinning-cube uniforms declared (unused) → Task 4 ✓
  - `GLSkybox` class + skybox shader pair → Task 5 ✓
  - Skybox pass in `endFrame` between lit and HUD → Task 5 ✓
  - Horizon fog blend in skybox shader → Task 5 ✓
  - Strandbound integration (fog config + procedural cubemap +
    `setSkybox` + shader update) → Task 6 ✓
  - Concept doc updated → Task 7 ✓
- [ ] Placeholder scan: no "TBD" / "implement later" / "similar to" /
  bare descriptions without code. ✓
- [ ] Type consistency:
  - `CubemapHandle` spelled the same in Task 1 (typedef), Task 3
    (RHI verb signatures), Task 5 (member), and Task 6 (caller). ✓
  - `kHorizonFogBand = 0.25f` matches the skybox shader's `uHorizonFogBand`
    uniform name; passed by the renderer in Task 5. ✓
  - `kInvalidHandle` used consistently as the "no skybox" sentinel. ✓
  - `Fog` field names `color`, `density` consistent across the struct
    (Task 1), the uniform names `uFogColor` / `uFogDensity` (Task 4 /
    Task 6), and the Strandbound config (Task 6). ✓
- [ ] Task 4's `beginFrame` signature matches across Renderer.h, the
  OpenGL header/impl, and both game callers — exactly one new parameter
  `const Fog& fog`, inserted between `pointLights` and `view`. ✓
- [ ] Task 6 references "the existing `lighting` variable" in the
  fragment shader (which lands at the bottom of `main()` after sun +
  shadow + point-light accumulation in the previous milestone) — the
  implementer should confirm by reading the file. ✓
