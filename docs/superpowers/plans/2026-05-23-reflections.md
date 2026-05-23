# Reflections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add cubemap environment reflections AND planar (mirrored-camera +
clip-plane) reflections to the engine, sharing a single per-`DrawCall`
reflectivity API. Strandbound becomes a sunset scene with water below the
floating islands plus a subtle sunset glaze on every surface.

**Architecture:** A new per-`DrawCall` `reflectivity` (float) and
`useReflectionPlane` (bool) drive reflection composition in the lit
fragment shader. When a reflection plane is set, the renderer adds a
planar reflection pass between shadow and lit that mirrors the camera,
applies a clip plane, and renders into an off-screen RTT. Reflective
surfaces sample either the planar RTT (screen-space UV) or the existing
sky cubemap (reflected view direction), based on the per-draw flag.

**Tech Stack:** C++23, OpenGL 3.3 (existing RHI), GLSL, stb_image (already
present), CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-23-reflections-design.md`.

**Task order rationale:** Each task leaves the build green. Tasks 1–5 are
additive (no behaviour change). Task 6 adds the reflection pass machinery
but it's a no-op until something is reflective. Task 7 wires the lit pass
to consume reflections (still no visual change: defaults are matte). Task 8
lights up Strandbound — the first user-visible change. Task 9 documents.

---

## Task 1: ReflectionPlane struct + reflectionMatrix math + unit tests

**Files:**
- Create: `engine/render/ReflectionPlane.h`
- Create: `engine/render/ReflectionPlane.cpp`
- Create: `tests/test_reflection.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)
- Modify: `engine/CMakeLists.txt` (add `ReflectionPlane.cpp`)

Purely additive — no existing code reads these symbols.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_reflection.cpp`:

```cpp
#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/ReflectionPlane.h"
#include "render/Renderer.h"  // for DrawCall

using namespace iron;

int main() {
    // 1. Reflection across y = 0: (1, 2, 3) -> (1, -2, 3).
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = 0.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{1.0f, 2.0f, 3.0f, 1.0f};
        CHECK_NEAR(p.x, 1.0f);
        CHECK_NEAR(p.y, -2.0f);
        CHECK_NEAR(p.z, 3.0f);
        CHECK_NEAR(p.w, 1.0f);
    }

    // 2. Reflection across y = -3 (normal {0,1,0}, d = -3):
    //    (0, 1, 0) is 4 units above the plane -> reflects to (0, -7, 0).
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, -7.0f);
        CHECK_NEAR(p.z, 0.0f);
    }

    // 3. The matrix is its own inverse: M * M = I.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Mat4 MM = M * M;
        Vec4 p = MM * Vec4{5.0f, 7.0f, 9.0f, 1.0f};
        CHECK_NEAR(p.x, 5.0f);
        CHECK_NEAR(p.y, 7.0f);
        CHECK_NEAR(p.z, 9.0f);
    }

    // 4. A point on the plane reflects to itself.
    //    For normal {0,1,0}, d = -3, the plane is y = -3.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{2.0f, -3.0f, 4.0f, 1.0f};
        CHECK_NEAR(p.x, 2.0f);
        CHECK_NEAR(p.y, -3.0f);
        CHECK_NEAR(p.z, 4.0f);
    }

    // 5. A point above and its reflection are equidistant from the plane.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 above = Vec4{0.0f, 5.0f, 0.0f, 1.0f};        // 8 above y=-3
        Vec4 below = M * above;
        CHECK_NEAR(above.y - (-3.0f), 8.0f);
        CHECK_NEAR((-3.0f) - below.y, 8.0f);
    }

    // 6. DrawCall reflection defaults.
    {
        DrawCall d;
        CHECK_NEAR(d.reflectivity, 0.0f);
        CHECK(d.useReflectionPlane == false);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
cmake --build build --config Debug 2>&1 | Select-String -Pattern "error"
```

Expected: fails because `ReflectionPlane`, `reflectionMatrix`,
`DrawCall::reflectivity`, and `DrawCall::useReflectionPlane` are not
defined. **Note:** the DrawCall fields are added in Task 3 — this test's
final assertion will not compile until Task 3 lands. For Task 1, you
have two options: (a) leave the DrawCall assertion in and accept that the
test file doesn't build until Task 3, OR (b) comment out the DrawCall
section in Task 1 and uncomment it in Task 3. **Use option (a)**: it's
fine to land a test file that doesn't compile until later in the
sequence, AS LONG AS the test binary isn't registered in CTest yet.

So for Task 1: do NOT add the test to CMake. Just write the file and the
struct + math. Registering the test happens at the end of Task 3 (when
all symbols exist).

- [ ] **Step 3: Create the header**

Create `engine/render/ReflectionPlane.h`:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"

namespace iron {

// A world-space mirror plane: every point on one side reflects to the
// other. `normal` must be unit-length; `d` is the signed distance to the
// origin along the normal. The plane is { p : dot(p, normal) = d }.
//
// Example: normal = {0,1,0}, d = -3 is the horizontal plane at y = -3.
struct ReflectionPlane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float d = 0.0f;
};

// Returns a 4x4 matrix that reflects any point across the plane. The
// matrix is its own inverse. Reflecting a point on the plane returns
// the same point.
Mat4 reflectionMatrix(const ReflectionPlane& plane);

} // namespace iron
```

- [ ] **Step 4: Create the implementation**

Create `engine/render/ReflectionPlane.cpp`:

```cpp
#include "render/ReflectionPlane.h"

namespace iron {

Mat4 reflectionMatrix(const ReflectionPlane& plane) {
    // The 3x3 mirror block is I - 2 * (n n^T). The translation column is
    // 2 * d * n: this shifts the mirror so the plane passes through the
    // point d * n instead of the origin.
    const float nx = plane.normal.x;
    const float ny = plane.normal.y;
    const float nz = plane.normal.z;

    Mat4 m;  // default-constructed: all zeros
    m.at(0, 0) = 1.0f - 2.0f * nx * nx;
    m.at(0, 1) = -2.0f * nx * ny;
    m.at(0, 2) = -2.0f * nx * nz;
    m.at(0, 3) = 2.0f * plane.d * nx;

    m.at(1, 0) = -2.0f * ny * nx;
    m.at(1, 1) = 1.0f - 2.0f * ny * ny;
    m.at(1, 2) = -2.0f * ny * nz;
    m.at(1, 3) = 2.0f * plane.d * ny;

    m.at(2, 0) = -2.0f * nz * nx;
    m.at(2, 1) = -2.0f * nz * ny;
    m.at(2, 2) = 1.0f - 2.0f * nz * nz;
    m.at(2, 3) = 2.0f * plane.d * nz;

    m.at(3, 3) = 1.0f;
    return m;
}

} // namespace iron
```

- [ ] **Step 5: Add to engine CMakeLists**

Open `engine/CMakeLists.txt`. Find the source list where the engine
library's .cpp files are added (look for other `engine/render/*.cpp` or
similar). Add `render/ReflectionPlane.cpp` to the list, matching the
established style (likely a `target_sources` block).

- [ ] **Step 6: Build to confirm the math compiles**

```powershell
cmake --build build --config Debug
```

Expected: clean build (the test file won't be in the build yet since we
haven't registered it).

- [ ] **Step 7: Commit (only the struct + math, no test wiring yet)**

```powershell
git add engine/render/ReflectionPlane.h engine/render/ReflectionPlane.cpp engine/CMakeLists.txt tests/test_reflection.cpp
git commit -m @'
Add ReflectionPlane struct and reflectionMatrix

reflectionMatrix(plane) returns a 4x4 mirror matrix: I - 2nn^T
for the 3x3 block, with 2d*n translation. The matrix is its
own inverse and a point on the plane maps to itself.

Also lands the unit tests as an unwired file; CMake registration
happens in the task that adds DrawCall::reflectivity (since one
of the tests references it).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, run `git log -1 --pretty=%s` and confirm subject is
exactly `Add ReflectionPlane struct and reflectionMatrix`.

---

## Task 2: appendQuad mesh helper

**Files:**
- Modify: `engine/scene/Mesh.h`
- Modify: `engine/scene/Mesh.cpp`

Adds a single-quad mesh builder alongside `appendBox` and `appendTube`.
Strandbound's water plane in Task 8 will use it.

- [ ] **Step 1: Declare the helper**

Open `engine/scene/Mesh.h`. Below the existing `appendBox` and
`appendTube` declarations, add:

```cpp
// Appends a single flat quad to `out`. The quad has size `size.x` along
// the local X axis and `size.y` along the local Z axis (a top-facing
// rectangle if `normal` is {0,1,0}). All four vertices share `normal`
// and UVs span 0..1 across the quad. Two triangles, CCW seen from
// `+normal`.
void appendQuad(MeshData& out, Vec3 center, Vec2 size, Vec3 normal);
```

Add `#include "math/Vec.h"` near the top if `Vec2` isn't already
reachable (likely it is via the existing structure).

- [ ] **Step 2: Implement the helper**

Open `engine/scene/Mesh.cpp`. Add at the bottom of the file (before the
closing `} // namespace iron`):

```cpp
void appendQuad(MeshData& out, Vec3 center, Vec2 size, Vec3 normal) {
    // Pick an in-plane "u" axis. Try +X first (most common for ground
    // planes); if normal is parallel to +X, fall back to +Y. Project
    // the hint into the plane and normalise.
    Vec3 uHint = (std::fabs(normal.x) < 0.99f) ? Vec3{1.0f, 0.0f, 0.0f}
                                               : Vec3{0.0f, 1.0f, 0.0f};
    const float along = dot(uHint, normal);
    Vec3 u = uHint - normal * along;
    const float ulen = std::sqrt(dot(u, u));
    u = (ulen > 1e-6f) ? u * (1.0f / ulen) : Vec3{1.0f, 0.0f, 0.0f};
    // v is in-plane, perpendicular to u; (u, v, normal) is right-handed.
    Vec3 v = cross(normal, u);

    const float hx = size.x * 0.5f;
    const float hy = size.y * 0.5f;

    // Four corners in CCW order seen from +normal:
    //   v0: -u, -v   v1: +u, -v   v2: +u, +v   v3: -u, +v
    Vec3 p0 = center + u * (-hx) + v * (-hy);
    Vec3 p1 = center + u * ( hx) + v * (-hy);
    Vec3 p2 = center + u * ( hx) + v * ( hy);
    Vec3 p3 = center + u * (-hx) + v * ( hy);

    const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());

    out.vertices.push_back(Vertex{p0, normal, Vec2{0.0f, 0.0f}});
    out.vertices.push_back(Vertex{p1, normal, Vec2{1.0f, 0.0f}});
    out.vertices.push_back(Vertex{p2, normal, Vec2{1.0f, 1.0f}});
    out.vertices.push_back(Vertex{p3, normal, Vec2{0.0f, 1.0f}});

    // Two triangles: (0, 1, 2) and (0, 2, 3).
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}
```

Add `#include <cmath>` if not already present.

If `Mesh.cpp` doesn't already use `cross`, `dot`, etc. from `Vec.h`, add
`#include "math/Vec.h"` to the top (it probably is included transitively
via `Mesh.h`).

- [ ] **Step 3: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build; 13/13 tests pass (no behaviour change yet).

- [ ] **Step 4: Commit**

```powershell
git add engine/scene/Mesh.h engine/scene/Mesh.cpp
git commit -m @'
Add appendQuad mesh builder

A flat rectangle in world space. Used by the reflections
milestone for Strandbound's water surface; useful elsewhere too
(walls, ground panels). Four vertices CCW around the normal,
UVs span 0..1, two triangles.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, run `git log -1 --pretty=%s` and confirm subject.

---

## Task 3: DrawCall::reflectivity + useReflectionPlane fields

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `tests/CMakeLists.txt` (register `test_reflection`)

Purely additive fields with zero defaults — every existing draw call
keeps its current behaviour. Also registers the test from Task 1 now that
all the symbols it references exist.

- [ ] **Step 1: Add the new fields to DrawCall**

Open `engine/render/Renderer.h`. Find the `DrawCall` struct (it currently
has `mesh`, `shader`, `texture`, `model`, `emissive`). Add two fields:

```cpp
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;        // 0 = matte, 1 = mirror
    bool useReflectionPlane = false;  // true: sample planar RTT; false: cubemap
};
```

Update the struct's doc comment if appropriate to mention the new
fields.

- [ ] **Step 2: Register the reflection test in CMake**

Open `tests/CMakeLists.txt`. Add a registration for `test_reflection.cpp`
matching the established style (look at how `test_point_lights` is added —
likely `iron_add_test(test_reflection test_reflection.cpp)` if a helper
macro exists, otherwise the verbose `add_executable` + `add_test` form).

- [ ] **Step 3: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build; **14/14** tests pass (the new `test_reflection`
binary runs its 6 internal checks).

- [ ] **Step 4: Commit**

```powershell
git add engine/render/Renderer.h tests/CMakeLists.txt
git commit -m @'
Add DrawCall reflectivity fields and wire reflection test

DrawCall gains float reflectivity (default 0 = matte) and bool
useReflectionPlane (default false = sample cubemap). Existing
draws keep current behaviour. Wires test_reflection into the
build now that all symbols it references exist.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

After committing, `git log -1 --pretty=%s`.

---

## Task 4: Renderer::setReflectionPlane + disableReflectionPlane

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`

Adds the RHI verbs to set/clear the active reflection plane. Stores the
plane on the OpenGL backend. No rendering yet.

- [ ] **Step 1: Add the virtuals to the Renderer interface**

Open `engine/render/Renderer.h`. Add `#include "render/ReflectionPlane.h"`
near the top alongside the other render-component includes. After
`setShadowBounds`, add:

```cpp
// Sets the world-space reflection plane. The renderer will run an extra
// planar reflection pass per frame using a camera mirrored across this
// plane; any DrawCall with useReflectionPlane=true samples the resulting
// texture in screen space. `normal` must be unit length.
virtual void setReflectionPlane(Vec3 normal, float d) = 0;

// Disables the planar reflection pass. Reflective DrawCalls with
// useReflectionPlane=true will then sample the cubemap as a fallback.
virtual void disableReflectionPlane() = 0;
```

- [ ] **Step 2: Add overrides + member to OpenGLRenderer.h**

Open `engine/render/backends/opengl/OpenGLRenderer.h`. Add the override
declarations alongside `setShadowBounds`:

```cpp
void setReflectionPlane(Vec3 normal, float d) override;
void disableReflectionPlane() override;
```

Add `#include <optional>` if not already present. Add a private member
to store the plane (use `std::optional` so "no plane set" is
unambiguous):

```cpp
std::optional<ReflectionPlane> reflectionPlane_;
```

Place it near `shadowCenter_` / `shadowRadius_` (other per-frame render
state).

- [ ] **Step 3: Implement the methods**

Open `engine/render/backends/opengl/OpenGLRenderer.cpp`. Add the
implementations alongside `setShadowBounds`:

```cpp
void OpenGLRenderer::setReflectionPlane(Vec3 normal, float d) {
    ReflectionPlane plane;
    plane.normal = normal;
    plane.d = d;
    reflectionPlane_ = plane;
}

void OpenGLRenderer::disableReflectionPlane() {
    reflectionPlane_.reset();
}
```

- [ ] **Step 4: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build; 14/14 pass.

- [ ] **Step 5: Commit**

```powershell
git add engine/render/Renderer.h engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp
git commit -m @'
Add Renderer::setReflectionPlane and disableReflectionPlane

The OpenGL backend stores the active plane in std::optional.
Neither method does rendering yet; the planar reflection pass
lands when GLReflectionTarget is wired in.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 5: GLReflectionTarget class

**Files:**
- Create: `engine/render/backends/opengl/GLReflectionTarget.h`
- Create: `engine/render/backends/opengl/GLReflectionTarget.cpp`
- Modify: `engine/CMakeLists.txt`

Wraps an FBO with a colour RTT and a depth RTT. Models on `GLShadowMap`.
Used by the planar reflection pass next task.

- [ ] **Step 1: Create the header**

Create `engine/render/backends/opengl/GLReflectionTarget.h`:

```cpp
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
```

- [ ] **Step 2: Create the implementation**

Create `engine/render/backends/opengl/GLReflectionTarget.cpp`:

```cpp
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
    if (!colorTexture_) {
        Log::warn("GLReflectionTarget::bindColorTexture on invalid target");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
}

} // namespace iron
```

- [ ] **Step 3: Add to CMakeLists**

Open `engine/CMakeLists.txt`. Add `render/backends/opengl/GLReflectionTarget.cpp`
to the engine library's source list alongside `GLCubemap.cpp` and
`GLSkybox.cpp`.

- [ ] **Step 4: Build and test**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build; 14/14 tests pass. No caller wires up
GLReflectionTarget yet, so no behavioural change.

- [ ] **Step 5: Commit**

```powershell
git add engine/render/backends/opengl/GLReflectionTarget.h engine/render/backends/opengl/GLReflectionTarget.cpp engine/CMakeLists.txt
git commit -m @'
Add GLReflectionTarget class

FBO with RGBA8 colour RTT and 24-bit depth RTT, both at the
same square resolution. Models on GLShadowMap. The planar
reflection pass in a later commit binds this for writing.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 6: Reflection-pass shaders + planar reflection pass in endFrame

**Files:**
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`

Adds the simplified reflection-pass shaders (inline strings) and the
planar reflection pass that runs between shadow and lit, only when
`reflectionPlane_` has a value. Skips DrawCalls with
`useReflectionPlane=true` to avoid the water reflecting itself.

- [ ] **Step 1: Add new members to OpenGLRenderer.h**

Open `engine/render/backends/opengl/OpenGLRenderer.h`. Add the include:

```cpp
#include "render/backends/opengl/GLReflectionTarget.h"
```

Add private members alongside the other GL helpers (near `shadowMap_`):

```cpp
GLReflectionTarget reflectionTarget_;
GLShader reflectionShader_;
```

- [ ] **Step 2: Initialise the new members in the constructor**

Open `engine/render/backends/opengl/OpenGLRenderer.cpp`. Find the
constructor (it currently initialises `shadowMap_` and `depthShader_` via
the member initialiser list). Add a constant near the top of the file
(in the anonymous namespace if there is one, or near the existing
`kShadowResolution`):

```cpp
constexpr int kReflectionResolution = 1024;
```

Add the reflection shader sources as anonymous-namespace constants near
the existing shader sources (or in the same anonymous namespace if it
already holds shader strings):

```cpp
namespace {

const char* kReflectionVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uReflectionView;
uniform mat4 uProjection;
uniform vec4 uClipPlane;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
void main() {
    vec4 worldPos4 = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos4.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_ClipDistance[0] = dot(worldPos4.xyz, uClipPlane.xyz) + uClipPlane.w;
    gl_Position = uProjection * uReflectionView * worldPos4;
}
)";

const char* kReflectionFragmentShader = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;
void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -normalize(uLightDir)), 0.0);
    vec3 lighting = uLightColor * (diffuse + uAmbient);
    vec4 texel = texture(uTexture, vUV);
    FragColor = vec4(texel.rgb * lighting, texel.a);
}
)";

} // namespace
```

Add the new members to the constructor's initialiser list:

```cpp
OpenGLRenderer::OpenGLRenderer()
    : shadowMap_(kShadowResolution),
      depthShader_(kDepthVertexShader, kDepthFragmentShader),
      reflectionTarget_(kReflectionResolution),                   // NEW
      reflectionShader_(kReflectionVertexShader,                  // NEW
                        kReflectionFragmentShader),               // NEW
      hud_() {
    // ... existing body
}
```

(Match the existing initialiser list order and style; the constants
`kDepthVertexShader` / `kDepthFragmentShader` may be inline in the .cpp
already.)

After the constructor body, you may want to log a warning if any of the
new GL resources failed to initialise:

```cpp
if (!reflectionTarget_.isValid()) {
    Log::warn("OpenGLRenderer: reflection target failed to initialise; "
              "planar reflections will be skipped");
}
if (!reflectionShader_.isValid()) {
    Log::warn("OpenGLRenderer: reflection shader failed to compile; "
              "planar reflections will be skipped");
}
```

- [ ] **Step 3: Add the reflection pass to endFrame**

In `OpenGLRenderer::endFrame`, between the existing **shadow depth pass**
(which ends with the viewport-restore and the framebuffer-unbind) and
the **lit pass** (which begins with `glClearColor` for the main
framebuffer), insert the planar reflection pass:

```cpp
// --- Pass 2: planar reflection (if a plane is set) ---
if (reflectionPlane_.has_value() &&
    reflectionTarget_.isValid() &&
    reflectionShader_.isValid()) {

    const ReflectionPlane& plane = *reflectionPlane_;
    const Mat4 mirror = reflectionMatrix(plane);
    const Mat4 reflectionView = view_ * mirror;

    // Clip-plane vector: (normal, -d). The vertex shader does
    // dot(pos, n) + w which is positive for points on the same side
    // as the normal — discard the rest.
    const Vec4 clipPlane{plane.normal.x, plane.normal.y, plane.normal.z,
                         -plane.d};

    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);

    reflectionTarget_.bindForWriting();
    glViewport(0, 0, reflectionTarget_.resolution(),
               reflectionTarget_.resolution());
    glClearColor(clearColor_.x, clearColor_.y, clearColor_.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_CLIP_DISTANCE0);

    reflectionShader_.bind();
    reflectionShader_.setMat4("uReflectionView", reflectionView);
    reflectionShader_.setMat4("uProjection", projection_);
    reflectionShader_.setVec3("uLightDir", light_.direction);
    reflectionShader_.setVec3("uLightColor", light_.color);
    reflectionShader_.setFloat("uAmbient", light_.ambient);
    reflectionShader_.setVec4("uClipPlane", clipPlane);
    reflectionShader_.setInt("uTexture", 0);

    for (const DrawCall& call : frameCalls_) {
        if (call.useReflectionPlane) continue;  // skip water itself
        reflectionShader_.setMat4("uModel", call.model);

        TextureHandle tex = call.texture;
        if (tex == kInvalidHandle) {
            tex = fallbackTexture_;
        }
        if (tex != kInvalidHandle && tex <= textures_.size()) {
            textures_[tex - 1]->bind(0);
        }

        meshes_[call.mesh - 1]->draw();
    }

    glDisable(GL_CLIP_DISTANCE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(savedViewport[0], savedViewport[1],
               savedViewport[2], savedViewport[3]);
}
```

**Important:** `GLShader` may not currently have `setVec4`. Check the
existing methods. If not, you have two options:
(a) Add `setVec4` to `GLShader.h/.cpp` (mirroring `setVec3`).
(b) Pass the clip plane as separate vec3 + float uniforms.

Use option (a). It's a 4-line addition to `GLShader.h`:

```cpp
void setVec4(const char* name, Vec4 v) const;
```

And to `GLShader.cpp`:

```cpp
void GLShader::setVec4(const char* name, Vec4 v) const {
    if (!program_) return;
    const GLint loc = glGetUniformLocation(program_, name);
    glUniform4f(loc, v.x, v.y, v.z, v.w);
}
```

(Match the existing setter pattern exactly.)

The skybox pass and HUD pass (which come later in `endFrame` or are
called by the game after `endFrame`) are unchanged.

- [ ] **Step 4: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 5: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 14/14 pass.

- [ ] **Step 6: Optional — launch games**

Neither game calls `setReflectionPlane` yet, so the reflection pass is
skipped. Both games should look identical to before. Skip if you can't
launch GUI apps.

- [ ] **Step 7: Commit**

```powershell
git add engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp engine/render/backends/opengl/GLShader.h engine/render/backends/opengl/GLShader.cpp
git commit -m @'
Add planar reflection pass in endFrame

When a reflection plane is set, render the scene from a
mirrored camera into GLReflectionTarget, with a clip plane
discarding fragments below the plane. Skip DrawCalls with
useReflectionPlane=true to avoid self-recursion. Uses the
simplified reflection shader (sun + ambient + texture only;
no shadows, no point lights, no fog, no emissive).

Also adds GLShader::setVec4 (mirrors setVec3) for the clip
plane uniform.

The pass is a no-op until a game calls setReflectionPlane.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 7: Lit pass — reflection uniform uploads + spinning-cube shader

**Files:**
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp`

Wires the lit pass to upload the reflection-related uniforms (cubemap
binding, planar RTT binding, per-draw reflectivity, screen size, camera
position). The spinning-cube fragment shader declares the new uniforms so
uploads don't silently no-op.

The Strandbound shader update lands in Task 8 along with the scene wiring.

- [ ] **Step 1: Add a static helper for camera-position extraction**

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, near the top (in
the anonymous namespace if there is one), add:

```cpp
// Extract the camera world position from a rigid view matrix (rotation
// + translation, no scale). For view = R * T(-camPos), the camera
// position is -R^T * (translation column).
Vec3 extractCameraPosition(const Mat4& view) {
    const float tx = view.at(0, 3);
    const float ty = view.at(1, 3);
    const float tz = view.at(2, 3);
    return Vec3{
        -(view.at(0, 0) * tx + view.at(1, 0) * ty + view.at(2, 0) * tz),
        -(view.at(0, 1) * tx + view.at(1, 1) * ty + view.at(2, 1) * tz),
        -(view.at(0, 2) * tx + view.at(1, 2) * ty + view.at(2, 2) * tz),
    };
}
```

- [ ] **Step 2: Cache camera position and screen size in beginFrame**

Add private members to `OpenGLRenderer.h`:

```cpp
Vec3 cameraPos_{};
int viewportWidth_ = 0;
int viewportHeight_ = 0;
```

In `setViewport(int width, int height)`, record `viewportWidth_ = width;
viewportHeight_ = height;` alongside the existing `glViewport` call.

At the end of `beginFrame`, after the existing per-frame state is
recorded, add:

```cpp
cameraPos_ = extractCameraPosition(view_);
```

- [ ] **Step 3: Upload reflection uniforms in the lit pass**

In `OpenGLRenderer::endFrame`, find the lit-pass per-draw loop. After
the existing point-light and emissive uniform uploads (and before the
texture binding / mesh draw), add:

```cpp
// Reflection uniforms — per-frame, but uploaded per-draw to match the
// existing sun/fog pattern.
shader.setVec3("uCameraPos", cameraPos_);
shader.setVec2("uScreenSize", Vec2{static_cast<float>(viewportWidth_),
                                   static_cast<float>(viewportHeight_)});

// Per-draw reflectivity.
shader.setFloat("uReflectivity", call.reflectivity);
shader.setInt("uUseReflectionPlane", call.useReflectionPlane ? 1 : 0);

// Bind the cubemap (if any) to unit 2 and the reflection RTT to unit 3.
// The shader's uSkyCubemap and uReflectionTexture samplers point to
// those units.
shader.setInt("uSkyCubemap", 2);
shader.setInt("uReflectionTexture", 3);

if (skybox_ != kInvalidHandle && skybox_ <= cubemaps_.size()) {
    cubemaps_[skybox_ - 1]->bind(2);
}
if (reflectionPlane_.has_value() && reflectionTarget_.isValid()) {
    reflectionTarget_.bindColorTexture(3);
}
```

**Important:** also disable `useReflectionPlane` in the shader when no
plane is set (so reflective fragments fall back to cubemap instead of
sampling an unbound texture). One clean way to do this: compute an
effective flag in C++:

```cpp
const int effectiveUsePlane =
    (call.useReflectionPlane && reflectionPlane_.has_value()) ? 1 : 0;
shader.setInt("uUseReflectionPlane", effectiveUsePlane);
```

Replace the earlier `call.useReflectionPlane ? 1 : 0` with this.

After the lit-pass loop, clean up the new texture units (mirror the
existing unit-1 cleanup):

```cpp
// Existing block cleans up unit 1 (shadow). Add cleanup for the new
// units we bound.
glActiveTexture(GL_TEXTURE2);
glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
glActiveTexture(GL_TEXTURE3);
glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(GL_TEXTURE0);
```

- [ ] **Step 4: Update the spinning-cube shader to declare new uniforms**

Open `games/01-spinning-cube/main.cpp`. Find `kFragmentShader`. In the
uniform declarations near the top, add (alongside the existing point-
light and fog uniforms):

```glsl
uniform samplerCube uSkyCubemap;
uniform sampler2D uReflectionTexture;
uniform float uReflectivity;
uniform int uUseReflectionPlane;
uniform vec2 uScreenSize;
uniform vec3 uCameraPos;
// (declared but unused — cube stays unlit textured)
```

The cube's `main()` body is unchanged. With `uReflectivity = 0` (the
default), the lit shader's reflection block (added in Task 8 for
Strandbound) is gated by `if (uReflectivity > 0.0)` — but the spinning
cube shader doesn't have that block. So declaring the uniforms here is
just to satisfy the C++ uniform-upload — they're literally unused in
shader code. Fine.

- [ ] **Step 5: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 6: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 14/14.

- [ ] **Step 7: Optional — launch games**

Both games should look identical to before. The cube has `reflectivity = 0`
(default); Strandbound's shader doesn't have the reflection block yet
(Task 8 adds it). Skip if you can't launch GUI apps.

- [ ] **Step 8: Commit**

```powershell
git add engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp games/01-spinning-cube/main.cpp
git commit -m @'
Upload reflection uniforms in the lit pass

Adds per-frame and per-draw reflection-related uniforms to the
lit pass: uCameraPos, uScreenSize, uReflectivity,
uUseReflectionPlane, uSkyCubemap (unit 2), uReflectionTexture
(unit 3). Caches cameraPos by extracting it from the view
matrix (rigid transform, so -R^T * t). Spinning-cube shader
declares the new uniforms so uploads target real locations;
cube stays unlit textured.

uUseReflectionPlane is forced to 0 when no plane is set so
reflective surfaces fall back to cubemap instead of sampling
an unbound texture.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Task 8: Light up Strandbound — water + cubemap glaze + shader reflection

**File:** `games/02-strandbound/main.cpp`

The user-visible payoff. Adds:
- A water plane mesh at y = -3 spanning 60×60 units.
- `renderer.setReflectionPlane({0,1,0}, -3.0f)` at startup.
- Per-frame water DrawCall with `reflectivity = 0.85f`, `useReflectionPlane = true`.
- Subtle `reflectivity = 0.08f`, `useReflectionPlane = false` on existing box / rope / anchor draws.
- Fragment shader gains the reflection composition step at the end.

- [ ] **Step 1: Update the fragment shader**

Open `games/02-strandbound/main.cpp`. Find `kFragmentShader`. Add the new
uniforms alongside the existing ones (after the fog uniforms):

```glsl
uniform samplerCube uSkyCubemap;
uniform sampler2D uReflectionTexture;
uniform float uReflectivity;
uniform int uUseReflectionPlane;
uniform vec2 uScreenSize;
uniform vec3 uCameraPos;
```

In `main()`, after the existing fog `mix` (which produces `finalColor`),
add the reflection composition:

```glsl
if (uReflectivity > 0.0) {
    vec3 reflectColor;
    if (uUseReflectionPlane == 1) {
        vec2 reflectUV = gl_FragCoord.xy / uScreenSize;
        reflectColor = texture(uReflectionTexture, reflectUV).rgb;
    } else {
        vec3 viewDir = normalize(vWorldPos - uCameraPos);
        vec3 reflectDir = reflect(viewDir, normalize(vNormal));
        reflectColor = texture(uSkyCubemap, reflectDir).rgb;
    }
    finalColor = mix(finalColor, reflectColor, uReflectivity);
}

FragColor = vec4(finalColor, texel.a);
```

(Replace the existing final `FragColor = vec4(finalColor, texel.a);` —
that line moves inside the new composition, after the reflection mix.)

- [ ] **Step 2: Register the reflection plane and build the water mesh**

In `main` (or wherever Strandbound's scene is initialised), early —
after `OpenGLRenderer renderer;` and the existing setup. Add:

```cpp
// Water plane at y = -3 visible from the edges of the floating islands.
renderer.setReflectionPlane(iron::Vec3{0.0f, 1.0f, 0.0f}, -3.0f);

iron::MeshData waterData;
iron::appendQuad(waterData,
                 iron::Vec3{0.0f, 0.0f, 0.0f},   // local origin
                 iron::Vec2{60.0f, 60.0f},        // 60x60 spans all islands
                 iron::Vec3{0.0f, 1.0f, 0.0f});   // upward-facing
iron::MeshHandle waterMesh = renderer.createMesh(waterData);
```

- [ ] **Step 3: Submit the water draw call per frame**

Inside the render lambda, AFTER the existing world / bulb draws (so the
water draws last among lit-pass objects), add:

```cpp
iron::DrawCall water;
water.mesh = waterMesh;
water.shader = shader;  // (or whatever the existing lit shader handle
                        // variable is — read the file)
water.texture = renderer.whiteTexture();
water.model = iron::translation(iron::Vec3{0.0f, -3.0f, 0.0f});
water.reflectivity = 0.85f;
water.useReflectionPlane = true;
renderer.submit(water);
```

- [ ] **Step 4: Add subtle cubemap glaze to existing land surfaces**

The boxes / anchor pole / ropes use existing DrawCall construction. Find
each draw-submit site for non-bulb world geometry and set:

```cpp
draw.reflectivity = 0.08f;
draw.useReflectionPlane = false;
```

For the **ropes**, set the same (subtle cubemap glaze) — read
`games/02-strandbound/RopeTool.cpp` or wherever the rope's DrawCall is
built. If rope DrawCalls live in a separate file, you may need to add
the fields there too.

For the **lantern bulbs**, leave `reflectivity = 0` (light sources are
emissive, not reflective).

Skip the **HUD** entirely — it's a separate pass.

- [ ] **Step 5: Build**

```powershell
cmake --build build --config Debug
```

Expected: clean build.

- [ ] **Step 6: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 14/14.

- [ ] **Step 7: Skip playtest — the user will playtest**

Don't launch the game. The user will playtest at the end of all tasks.

- [ ] **Step 8: Commit**

```powershell
git add games/02-strandbound/main.cpp games/02-strandbound/RopeTool.cpp
git commit -m @'
Light up Strandbound with reflections

Adds a 60x60 water plane at y = -3 with reflectivity = 0.85
sampling the planar RTT. Sets the reflection plane to {0,1,0,-3}
at startup. Subtle cubemap glaze (reflectivity = 0.08) on the
boxes / anchor pole / ropes so the sunset shows on every land
surface. Lantern bulbs stay non-reflective (emissive only).
Fragment shader gains the reflection composition step at the
end of main, after fog.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

(If `RopeTool.cpp` doesn't change, drop it from the `git add` — the
content of the rope draw lives in `main.cpp` and that's the only file
modified.)

---

## Task 9: Update lighting docs

**File:** `docs/engine/lighting.md`

Append a short section on reflections. Matches the file's tone (narrative
concept paragraphs, not code walkthroughs).

- [ ] **Step 1: Read the existing doc**

(Use the `Read` tool.)

- [ ] **Step 2: Append the Reflections section**

After the existing `## Atmosphere` section and before `## Normals and
scaling`, insert:

```markdown
## Reflections

Two complementary reflection mechanisms share one material API.
`DrawCall::reflectivity` (0..1) decides how much reflected colour is
mixed on top of the fogged lit colour; `DrawCall::useReflectionPlane`
decides where the reflected colour comes from.

**Cubemap environment reflections** (`useReflectionPlane = false`) sample
the active sky cubemap based on the reflected view direction
(`reflect(viewDir, normal)`). Every surface picks up a hint of the sky —
on the sunset Strandbound scene this manifests as a warm orange glaze.
Cheap (one extra texture sample) and runs everywhere automatically.

**Planar reflections** (`useReflectionPlane = true`) sample a separate
off-screen render target in screen space. Once per frame, the renderer
mirrors the camera across a single registered world-space plane and
re-renders the scene with a clip plane that discards everything below
the mirror surface. The mirrored render runs through a simplified
shader (sun + ambient + texture only — no shadows, no point lights, no
fog, no emissive, no recursion) so it's cheap relative to the full lit
pass. Reflective surfaces above the plane sample this RTT to show real
geometry. Strandbound uses one plane for water around the floating
islands.

The two mechanisms cooperate: a surface that fails to find planar
reflections (no plane set, or the plane is invalid) falls back
automatically to the cubemap. Future milestones may add Fresnel
(view-angle-dependent reflectivity), roughness (blurred reflections),
or multiple planes — for now, one plane, one reflectivity scalar per
draw call.
```

- [ ] **Step 3: Commit**

```powershell
git add docs/engine/lighting.md
git commit -m @'
Document reflections (cubemap env + planar)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
'@
```

---

## Self-review checklist (the planner's)

After implementation, the planner verifies:

- [ ] Spec coverage:
  - `ReflectionPlane` + `reflectionMatrix` → Task 1 ✓
  - `appendQuad` helper → Task 2 ✓
  - `DrawCall::reflectivity` + `useReflectionPlane` → Task 3 ✓
  - `Renderer::setReflectionPlane` + `disableReflectionPlane` → Task 4 ✓
  - `GLReflectionTarget` → Task 5 ✓
  - Planar reflection pass + reflection-pass shader pair → Task 6 ✓
  - `GLShader::setVec4` (needed for clip plane uniform) → Task 6 ✓
  - Lit pass reflection uniform uploads (cubemap unit 2, RTT unit 3,
    reflectivity, useReflectionPlane, screenSize, cameraPos) → Task 7 ✓
  - Spinning-cube shader uniform declarations → Task 7 ✓
  - Strandbound shader reflection composition → Task 8 ✓
  - Strandbound water mesh + reflection plane + DrawCall config → Task 8 ✓
  - Strandbound box/rope subtle glaze → Task 8 ✓
  - Doc update → Task 9 ✓
  - Unit tests for reflectionMatrix + DrawCall defaults → Task 1 ✓

- [ ] Placeholder scan: no "TBD" / "implement later" / "similar to" /
  bare descriptions without code. ✓

- [ ] Type consistency:
  - `ReflectionPlane { Vec3 normal; float d; }` spelled the same in
    Task 1 (definition), Task 4 (`setReflectionPlane` signature uses
    `Vec3 normal, float d` then constructs internally), Task 6
    (consumed in endFrame). ✓
  - `reflectionMatrix(plane)` returns `Mat4` — consistent with the math
    helper's signature. ✓
  - `kReflectionResolution = 1024` (anonymous namespace constant) used
    consistently. ✓
  - `useReflectionPlane` spelled the same on `DrawCall` (Task 3), in the
    skip condition in Task 6, in the effective-flag computation in
    Task 7, and in the Strandbound draw in Task 8. ✓
  - `uReflectivity`, `uUseReflectionPlane`, `uSkyCubemap`,
    `uReflectionTexture`, `uScreenSize`, `uCameraPos` — same uniform
    names across spinning-cube shader (Task 7), Strandbound shader
    (Task 8), and renderer uploads (Task 7). ✓

- [ ] No "similar to Task N" — Task 1's test file is self-contained;
  Task 8's shader edits include the full new GLSL block verbatim. ✓

- [ ] Task 6's `setVec4` addition to GLShader is called out as a
  prerequisite; the implementer adds it as part of the same task rather
  than as a separate task. ✓
