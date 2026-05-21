# Strandbound M3 — "Ropes that Swing" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a general Verlet physics module and debug-line rendering to the Iron Core Engine, then give the Strandbound game a rope that hangs from a pole and swings as the player drags one end around.

**Architecture:** Builds on the completed M1/M2 engine. M3 adds: a header-only Verlet physics core (`VerletPoint` + integration, `DistanceConstraint` + satisfaction) and a `Rope` helper built on it; and debug-line rendering on the RHI (`drawLine` / `flushDebugLines`) with an OpenGL backend. The Strandbound game gets a pole and one rope whose free end follows the player; the rope is drawn with debug lines.

**Tech Stack:** C++23, CMake, MSVC, OpenGL 3.3 (via the existing RHI). No new third-party dependencies.

**Conventions:**
- Namespace `iron`. Engine headers included relative to `engine/`. `Mat4` column-major.
- Build: `cmake -S . -B build` then `cmake --build build`.
- Tests (MSVC multi-config): `ctest --test-dir build -C Debug --output-on-failure`.
- Commit after every task; commit messages end with the `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- Spec: `docs/superpowers/specs/2026-05-21-strandbound-m3-ropes-that-swing-design.md`.

---

## File Structure

**Created by this plan:**

```
engine/physics/VerletPoint.h                       VerletPoint struct + integrate()
engine/physics/DistanceConstraint.h                DistanceConstraint struct + satisfy()
engine/physics/Rope.h, Rope.cpp                    Rope helper
engine/render/backends/opengl/GLDebugLines.h/.cpp  dynamic line batch + flat-color shader
tests/test_verlet.cpp                              physics unit tests
docs/engine/rope-physics.md                        concept note
```

**Modified by this plan:**

```
engine/render/Renderer.h                            drawLine + flushDebugLines (RHI)
engine/render/backends/opengl/OpenGLRenderer.h/.cpp  debug-line implementation
engine/CMakeLists.txt                               add Rope.cpp + GLDebugLines.cpp
tests/CMakeLists.txt                                register test_verlet
games/02-strandbound/main.cpp                       pole + rope + debug-line draw
```

**Spec refinement note:** the spec describes the `Rope` as "constructed with two endpoint positions and a segment count." This plan adds a fourth constructor parameter — the rope's natural length — so the rope can carry slack and visibly *dangle* (the spec's own acceptance criterion). A rope whose natural length equals the endpoint distance is a taut string that never sags.

---

## Task 1: VerletPoint and integration

**Files:**
- Create: `tests/test_verlet.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `engine/physics/VerletPoint.h`

- [ ] **Step 1: Write the failing test `tests/test_verlet.cpp`**

```cpp
#include "test_framework.h"
#include "math/Vec.h"
#include "physics/VerletPoint.h"

using namespace iron;

int main() {
    // A point at rest with no acceleration stays put.
    {
        VerletPoint p;
        p.position = Vec3{1.0f, 2.0f, 3.0f};
        p.previousPosition = Vec3{1.0f, 2.0f, 3.0f};
        integrate(p, Vec3{0.0f, 0.0f, 0.0f}, 0.1f);
        CHECK_NEAR(p.position.x, 1.0f);
        CHECK_NEAR(p.position.y, 2.0f);
        CHECK_NEAR(p.position.z, 3.0f);
    }

    // One step under gravity moves the point by acceleration * dt * dt.
    {
        VerletPoint p;
        p.position = Vec3{0.0f, 0.0f, 0.0f};
        p.previousPosition = Vec3{0.0f, 0.0f, 0.0f};
        integrate(p, Vec3{0.0f, -10.0f, 0.0f}, 0.5f);
        CHECK_NEAR(p.position.y, -2.5f);  // -10 * 0.5 * 0.5
    }

    // Implicit velocity carries the point forward (inertia), and
    // previousPosition rolls forward to the old position.
    {
        VerletPoint p;
        p.position = Vec3{1.0f, 0.0f, 0.0f};
        p.previousPosition = Vec3{0.0f, 0.0f, 0.0f};  // moving +X by 1 per step
        integrate(p, Vec3{0.0f, 0.0f, 0.0f}, 0.1f);
        CHECK_NEAR(p.position.x, 2.0f);
        CHECK_NEAR(p.previousPosition.x, 1.0f);
    }

    // A pinned point never moves under integration.
    {
        VerletPoint p;
        p.position = Vec3{5.0f, 5.0f, 5.0f};
        p.previousPosition = Vec3{5.0f, 5.0f, 5.0f};
        p.pinned = true;
        integrate(p, Vec3{0.0f, -10.0f, 0.0f}, 1.0f);
        CHECK_NEAR(p.position.y, 5.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

Add after the existing `iron_add_test(test_first_person_controller test_first_person_controller.cpp)` line:

```cmake
iron_add_test(test_verlet test_verlet.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `physics/VerletPoint.h` does not exist.

- [ ] **Step 4: Write `engine/physics/VerletPoint.h`**

```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// A mass point for Verlet integration. It stores no explicit velocity —
// velocity is implicit in (position - previousPosition). A pinned point is an
// anchor: integration never moves it.
struct VerletPoint {
    Vec3 position;
    Vec3 previousPosition;
    bool pinned = false;
};

// Advance one point by a single Verlet step under a constant acceleration.
// Pinned points are left untouched.
//
//   velocity   = position - previousPosition   (implicit)
//   next       = position + velocity + acceleration * dt*dt
//
inline void integrate(VerletPoint& p, Vec3 acceleration, float dt) {
    if (p.pinned) {
        return;
    }
    const Vec3 velocity = p.position - p.previousPosition;
    const Vec3 next = p.position + velocity + acceleration * (dt * dt);
    p.previousPosition = p.position;
    p.position = next;
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 6 tests pass, including `test_verlet`.

- [ ] **Step 6: Commit**

```bash
git add engine/physics/VerletPoint.h tests/test_verlet.cpp tests/CMakeLists.txt
git commit -m "Add VerletPoint and Verlet integration with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 2: DistanceConstraint

**Files:**
- Create: `engine/physics/DistanceConstraint.h`
- Modify: `tests/test_verlet.cpp`

- [ ] **Step 1: Write `engine/physics/DistanceConstraint.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "physics/VerletPoint.h"

#include <cstddef>
#include <vector>

namespace iron {

// Keeps two points — referenced by index into a point array — a fixed
// distance apart. This is what gives a rope its stiffness.
struct DistanceConstraint {
    int a = 0;
    int b = 0;
    float restLength = 0.0f;
};

// Nudge the two constrained points toward restLength. Each free point takes
// half the correction; if one point is pinned, the free point takes all of
// it and the pinned point stays put. If both are pinned, nothing moves.
inline void satisfy(const DistanceConstraint& c, std::vector<VerletPoint>& points) {
    VerletPoint& pa = points[static_cast<std::size_t>(c.a)];
    VerletPoint& pb = points[static_cast<std::size_t>(c.b)];

    const Vec3 delta = pb.position - pa.position;
    const float dist = length(delta);
    if (dist <= 1e-8f) {
        return;  // degenerate: the points coincide, no defined direction
    }

    // `correction` points from a toward b; positive when the points are too
    // far apart, negative when too close.
    const float scale = (dist - c.restLength) / dist;
    const Vec3 correction = delta * scale;

    if (pa.pinned && pb.pinned) {
        return;
    }
    if (pa.pinned) {
        pb.position = pb.position - correction;
    } else if (pb.pinned) {
        pa.position = pa.position + correction;
    } else {
        pa.position = pa.position + correction * 0.5f;
        pb.position = pb.position - correction * 0.5f;
    }
}

} // namespace iron
```

- [ ] **Step 2: Add constraint tests to `tests/test_verlet.cpp`**

Add this include after the existing `#include "physics/VerletPoint.h"` line:

```cpp
#include "physics/DistanceConstraint.h"
```

Add these four test blocks immediately before the `return iron_test_result();` line:

```cpp
    // A stretched constraint pulls two free points back to rest length,
    // each moving equally toward the centre.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[1].position = Vec3{10.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 4.0f};
        satisfy(c, pts);
        CHECK_NEAR(length(pts[1].position - pts[0].position), 4.0f);
        CHECK_NEAR(pts[0].position.x, 3.0f);
        CHECK_NEAR(pts[1].position.x, 7.0f);
    }

    // A compressed constraint pushes two free points apart to rest length.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[1].position = Vec3{1.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 5.0f};
        satisfy(c, pts);
        CHECK_NEAR(length(pts[1].position - pts[0].position), 5.0f);
    }

    // With one endpoint pinned, only the free point moves — by the full
    // correction — and the pinned point stays exactly put.
    {
        std::vector<VerletPoint> pts(2);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[0].pinned = true;
        pts[1].position = Vec3{10.0f, 0.0f, 0.0f};
        DistanceConstraint c{0, 1, 4.0f};
        satisfy(c, pts);
        CHECK_NEAR(pts[0].position.x, 0.0f);
        CHECK_NEAR(pts[1].position.x, 4.0f);
    }

    // Iterating a chain of constraints converges every segment to rest length.
    {
        std::vector<VerletPoint> pts(3);
        pts[0].position = Vec3{0.0f, 0.0f, 0.0f};
        pts[0].pinned = true;
        pts[1].position = Vec3{5.0f, 0.0f, 0.0f};
        pts[2].position = Vec3{20.0f, 0.0f, 0.0f};
        DistanceConstraint c01{0, 1, 2.0f};
        DistanceConstraint c12{1, 2, 2.0f};
        for (int i = 0; i < 50; ++i) {
            satisfy(c01, pts);
            satisfy(c12, pts);
        }
        CHECK_NEAR(length(pts[1].position - pts[0].position), 2.0f);
        CHECK_NEAR(length(pts[2].position - pts[1].position), 2.0f);
    }
```

- [ ] **Step 3: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 6 tests pass; `test_verlet` now covers integration and constraints.

- [ ] **Step 4: Commit**

```bash
git add engine/physics/DistanceConstraint.h tests/test_verlet.cpp
git commit -m "Add DistanceConstraint with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 3: Rope

**Files:**
- Create: `engine/physics/Rope.h`
- Create: `engine/physics/Rope.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/test_verlet.cpp`

- [ ] **Step 1: Add rope tests to `tests/test_verlet.cpp`**

Add this include after the existing `#include "physics/DistanceConstraint.h"` line:

```cpp
#include "physics/Rope.h"
```

Add these three test blocks immediately before the `return iron_test_result();` line:

```cpp
    // A rope has segments + 1 points, and both ends are pinned.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 4, 10.0f);
        CHECK(static_cast<int>(rope.points().size()) == 5);
        CHECK(rope.points().front().pinned);
        CHECK(rope.points().back().pinned);
    }

    // The pinned endpoints stay where setEndpoint puts them, even after update.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 8, 12.0f);
        rope.setEndpointA(Vec3{1.0f, 5.0f, 0.0f});
        rope.setEndpointB(Vec3{9.0f, 5.0f, 0.0f});
        rope.update(1.0f / 60.0f);
        CHECK_NEAR(rope.points().front().position.x, 1.0f);
        CHECK_NEAR(rope.points().front().position.y, 5.0f);
        CHECK_NEAR(rope.points().back().position.x, 9.0f);
        CHECK_NEAR(rope.points().back().position.y, 5.0f);
    }

    // A slack rope's interior sags downward under gravity over time.
    {
        Rope rope(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f}, 8, 20.0f);
        const float startY = rope.points()[4].position.y;  // middle point
        for (int i = 0; i < 30; ++i) {
            rope.update(1.0f / 60.0f);
        }
        CHECK(rope.points()[4].position.y < startY);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: build FAILS — `physics/Rope.h` does not exist.

- [ ] **Step 3: Write `engine/physics/Rope.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "physics/DistanceConstraint.h"
#include "physics/VerletPoint.h"

#include <vector>

namespace iron {

// A rope simulated as a chain of VerletPoints linked by DistanceConstraints.
// Both ends are pinned anchors — reposition them each frame with
// setEndpointA / setEndpointB. The middle hangs and swings under gravity.
class Rope {
public:
    // Builds `segments` segments (segments + 1 points) evenly spaced along the
    // line from `endA` to `endB`. `ropeLength` is the rope's natural length:
    // make it larger than the distance between the endpoints to give the rope
    // slack so it visibly dangles. Both endpoints start pinned.
    Rope(Vec3 endA, Vec3 endB, int segments, float ropeLength);

    void setEndpointA(Vec3 position);  // moves the first point
    void setEndpointB(Vec3 position);  // moves the last point

    // Advance the simulation one fixed step: integrate every point under
    // gravity, then satisfy the distance constraints `iterations_` times.
    void update(float dt);

    const std::vector<VerletPoint>& points() const { return points_; }

private:
    std::vector<VerletPoint> points_;
    std::vector<DistanceConstraint> constraints_;
    Vec3 gravity_{0.0f, -9.8f, 0.0f};
    int iterations_ = 16;
};

} // namespace iron
```

- [ ] **Step 4: Write `engine/physics/Rope.cpp`**

```cpp
#include "physics/Rope.h"

#include <cstddef>

namespace iron {

Rope::Rope(Vec3 endA, Vec3 endB, int segments, float ropeLength) {
    if (segments < 1) {
        segments = 1;
    }
    const int pointCount = segments + 1;

    // Lay the points out in a straight line between the endpoints; gravity
    // pulls the slack into a hanging curve over the first few updates.
    points_.resize(static_cast<std::size_t>(pointCount));
    for (int i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const Vec3 p = endA + (endB - endA) * t;
        points_[static_cast<std::size_t>(i)].position = p;
        points_[static_cast<std::size_t>(i)].previousPosition = p;
    }
    points_.front().pinned = true;
    points_.back().pinned = true;

    // restLength is derived from the rope's natural length, NOT the endpoint
    // distance — that is what lets the rope carry slack.
    const float restLength = ropeLength / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        constraints_.push_back(DistanceConstraint{i, i + 1, restLength});
    }
}

void Rope::setEndpointA(Vec3 position) {
    points_.front().position = position;
}

void Rope::setEndpointB(Vec3 position) {
    points_.back().position = position;
}

void Rope::update(float dt) {
    for (VerletPoint& p : points_) {
        integrate(p, gravity_, dt);
    }
    // More iterations make the rope stiffer and less stretchy.
    for (int iter = 0; iter < iterations_; ++iter) {
        for (const DistanceConstraint& c : constraints_) {
            satisfy(c, points_);
        }
    }
}

} // namespace iron
```

- [ ] **Step 5: Add `physics/Rope.cpp` to `engine/CMakeLists.txt`**

Read `engine/CMakeLists.txt`. Add `physics/Rope.cpp` to the `add_library(ironcore STATIC ...)` source list, after `scene/FirstPersonController.cpp`:

```cmake
  scene/Camera.cpp
  scene/FirstPersonController.cpp
  physics/Rope.cpp
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 6 tests pass; `test_verlet` now also covers the rope.

- [ ] **Step 7: Commit**

```bash
git add engine/physics/Rope.h engine/physics/Rope.cpp engine/CMakeLists.txt tests/test_verlet.cpp
git commit -m "Add Rope built on the Verlet primitives, with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 4: Debug-line rendering

Adds `drawLine` / `flushDebugLines` to the RHI and implements them in the
OpenGL backend with a `GLDebugLines` helper. No unit test — rendering is
verified visually in Task 5.

**Files:**
- Create: `engine/render/backends/opengl/GLDebugLines.h`
- Create: `engine/render/backends/opengl/GLDebugLines.cpp`
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Write `engine/render/backends/opengl/GLDebugLines.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/backends/opengl/GLShader.h"

#include <cstdint>
#include <vector>

namespace iron {

// Accumulates coloured 3D line segments and draws them in one batch with
// GL_LINES. The vertex buffer is re-uploaded on every flush because the line
// set is transient (rebuilt each frame). Requires a current GL context.
class GLDebugLines {
public:
    GLDebugLines();
    ~GLDebugLines();

    GLDebugLines(const GLDebugLines&) = delete;
    GLDebugLines& operator=(const GLDebugLines&) = delete;

    // Queue one line segment for the current frame.
    void addLine(Vec3 a, Vec3 b, Vec3 color);

    // Upload the queued vertices, draw them, and clear the queue.
    void flush(const Mat4& view, const Mat4& projection);

private:
    struct Vertex {
        Vec3 position;
        Vec3 color;
    };

    std::vector<Vertex> vertices_;
    GLShader shader_;
    std::uint32_t vao_ = 0;
    std::uint32_t vbo_ = 0;
};

} // namespace iron
```

- [ ] **Step 2: Write `engine/render/backends/opengl/GLDebugLines.cpp`**

```cpp
#include "render/backends/opengl/GLDebugLines.h"

#include <glad/gl.h>

#include <cstddef>

namespace iron {

namespace {
const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform mat4 uViewProjection;

out vec3 vColor;

void main() {
    vColor = aColor;
    gl_Position = uViewProjection * vec4(aPos, 1.0);
}
)";

const char* kFragmentSrc = R"(#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 1.0);
}
)";
}  // namespace

GLDebugLines::GLDebugLines() : shader_(kVertexSrc, kFragmentSrc) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, color)));

    glBindVertexArray(0);
}

GLDebugLines::~GLDebugLines() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void GLDebugLines::addLine(Vec3 a, Vec3 b, Vec3 color) {
    vertices_.push_back(Vertex{a, color});
    vertices_.push_back(Vertex{b, color});
}

void GLDebugLines::flush(const Mat4& view, const Mat4& projection) {
    if (vertices_.empty()) {
        return;
    }

    shader_.bind();
    shader_.setMat4("uViewProjection", projection * view);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)),
                 vertices_.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_.size()));
    glBindVertexArray(0);

    vertices_.clear();
}

} // namespace iron
```

- [ ] **Step 3: Add the debug-line methods to the RHI interface `Renderer.h`**

In `engine/render/Renderer.h`, add two pure-virtual methods to the `Renderer`
class, immediately after the `endFrame()` declaration:

```cpp
    virtual void endFrame() = 0;

    // --- debug drawing ---
    // Queue a coloured 3D line segment for the current frame.
    virtual void drawLine(Vec3 a, Vec3 b, Vec3 color) = 0;
    // Draw all queued debug lines (depth-tested) and clear the queue.
    virtual void flushDebugLines(const Mat4& view, const Mat4& projection) = 0;
```

- [ ] **Step 4: Update `OpenGLRenderer.h`**

In `engine/render/backends/opengl/OpenGLRenderer.h`, add the include near the
other backend includes:

```cpp
#include "render/backends/opengl/GLDebugLines.h"
```

Add the two override declarations after the `endFrame()` override:

```cpp
    void endFrame() override;

    void drawLine(Vec3 a, Vec3 b, Vec3 color) override;
    void flushDebugLines(const Mat4& view, const Mat4& projection) override;
```

Add a private member after `light_`:

```cpp
    DirectionalLight light_{};
    GLDebugLines debugLines_;
```

- [ ] **Step 5: Update `OpenGLRenderer.cpp`**

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, add these two
definitions after the `endFrame()` definition:

```cpp
void OpenGLRenderer::drawLine(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.addLine(a, b, color);
}

void OpenGLRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    debugLines_.flush(view, projection);
}
```

- [ ] **Step 6: Add `GLDebugLines.cpp` to `engine/CMakeLists.txt`**

Add `render/backends/opengl/GLDebugLines.cpp` to the `add_library(ironcore
STATIC ...)` source list, after `render/backends/opengl/OpenGLRenderer.cpp`:

```cmake
  render/backends/opengl/OpenGLRenderer.cpp
  render/backends/opengl/GLDebugLines.cpp
```

- [ ] **Step 7: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean — `ironcore`, both games, and all test executables.
(`OpenGLRenderer` is still concrete: the two new pure-virtuals are overridden.)

- [ ] **Step 8: Commit**

```bash
git add engine
git commit -m "Add debug-line rendering to the renderer"
```
(plus the `Co-Authored-By` trailer)

---

## Task 5: A rope in the Strandbound game

**Files:**
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Add the rope include to `games/02-strandbound/main.cpp`**

Add after the existing `#include "physics/..."` group — specifically after
`#include "render/backends/opengl/OpenGLRenderer.h"` add the physics include
alongside the other engine includes:

```cpp
#include "physics/Rope.h"
```

(Place it so the include list stays roughly alphabetical; exact position does
not matter as long as it compiles.)

- [ ] **Step 2: Add a pole to the scene**

In `main()`, after the existing `scene.objects.push_back(...)` calls that build
the islands and props, add one more — a tall thin pole standing on the home
island (its base at the island top `y = 0`, so a height-4 pole is centred at
`y = 2`):

```cpp
    scene.objects.push_back(makeBox(iron::Vec3{5.0f, 2.0f, 0.0f},
                                    iron::Vec3{0.4f, 4.0f, 0.4f},
                                    cube, texture));  // rope pole
```

- [ ] **Step 3: Create the rope**

After the `FirstPersonController player;` setup block (after its setters), add:

```cpp
    // The rope hangs from the top of the pole; its other end follows the
    // player. ropeLength (16) exceeds the pole-to-player distance, so the
    // rope carries slack and visibly dangles.
    const iron::Vec3 poleTop{5.0f, 4.0f, 0.0f};
    const iron::Vec3 playerAnchorStart =
        player.position() + iron::Vec3{0.0f, 1.0f, 0.0f};
    iron::Rope rope(poleTop, playerAnchorStart, 24, 16.0f);
```

- [ ] **Step 4: Drive the rope in the update callback**

Inside the `app.setUpdate(...)` lambda, after the existing
`player.update(ci, time.deltaSeconds);` line, add:

```cpp
        // The rope's fixed end stays on the pole; its free end rides with the
        // player at roughly waist height.
        rope.setEndpointA(poleTop);
        rope.setEndpointB(player.position() + iron::Vec3{0.0f, 1.0f, 0.0f});
        rope.update(time.deltaSeconds);
```

- [ ] **Step 5: Draw the rope in the render callback**

Inside the `app.setRender(...)` lambda, replace the existing object-submit
loop and `endFrame()` so the rope is drawn between submitting the scene and
ending the frame. The render lambda body becomes:

```cpp
        renderer.beginFrame(iron::Vec3{0.5f, 0.7f, 0.9f}, scene.light);
        const iron::Mat4 view = player.viewMatrix();
        for (const iron::RenderObject& obj : scene.objects) {
            iron::DrawCall call;
            call.mesh = obj.mesh;
            call.shader = shader;
            call.texture = obj.texture;
            call.model = obj.transform;
            renderer.submit(call, view, projection);
        }

        // Draw the rope as one debug line per segment.
        const std::vector<iron::VerletPoint>& ropePoints = rope.points();
        for (std::size_t i = 0; i + 1 < ropePoints.size(); ++i) {
            renderer.drawLine(ropePoints[i].position,
                              ropePoints[i + 1].position,
                              iron::Vec3{0.55f, 0.35f, 0.18f});
        }
        renderer.flushDebugLines(view, projection);

        renderer.endFrame();
```

- [ ] **Step 6: Add the `<cstddef>` include**

`std::size_t` is used in the render loop above. In
`games/02-strandbound/main.cpp`, ensure `#include <cstddef>` is present in the
include block (add it if it is not).

- [ ] **Step 7: Build**

Run: `cmake --build build`
Expected: builds clean; `strandbound.exe` is produced.

- [ ] **Step 8: Run — milestone acceptance check**

Run: `build/games/02-strandbound/Debug/strandbound.exe`

Expected — **this is the M3 acceptance check**:
- A rope hangs from the pole on the island, drawn as a connected brown line.
- As the player walks around, the rope's free end follows them: the rope
  swings, drags behind, pulls taut when the player moves away from the pole,
  and goes slack and sags when they move closer.
- The motion is smooth and stable — the rope does not jitter apart, explode,
  or stretch without limit.
- `Escape` still quits.

> Visual verification is done by the controller / user, not an implementer
> subagent. If running the game blocks, just confirm it builds and launches
> without an immediate crash.

- [ ] **Step 9: Commit**

```bash
git add games/02-strandbound/main.cpp
git commit -m "MILESTONE M3: a swinging rope in the Strandbound game"
```
(plus the `Co-Authored-By` trailer)

---

## Task 6: Rope-physics concept note

**Files:**
- Create: `docs/engine/rope-physics.md`

- [ ] **Step 1: Write `docs/engine/rope-physics.md`**

```markdown
# Rope Physics

A rope in Iron Core is not animated — it is **simulated**. It is a chain of
mass points held together by constraints, and it falls, swings, and drapes on
its own.

## Verlet integration

Each point is a [[VerletPoint]]: it stores its current position and its
*previous* position — and nothing else. Velocity is never stored; it is
implied by the gap between the two:

\`\`\`
velocity = position - previousPosition
next     = position + velocity + acceleration * dt*dt
\`\`\`

This is **Verlet integration**. Storing the previous position instead of a
velocity has a useful side effect: when a constraint later *moves* a point,
that displacement silently becomes velocity next step. Collisions and
constraints "just work" without bookkeeping.

A **pinned** point is an anchor — integration skips it. The rope's two ends
are pinned.

## Distance constraints

On its own, a cloud of falling points is not a rope. A `DistanceConstraint`
ties two points together at a fixed *rest length*. Satisfying it measures the
current distance and nudges the points back toward the rest length — each
moves half way, unless one is pinned (then the free one moves all the way).

## Relaxation

One pass of constraint satisfaction is not enough — fixing one segment
disturbs its neighbour. So each frame we satisfy every constraint **many times
in a loop** (relaxation). More iterations → a stiffer, less stretchy rope;
fewer → a loose, elastic one.

## Slack

The rope only *dangles* if its natural length is longer than the straight-line
distance between its endpoints. That extra length is slack, and gravity pulls
it into a hanging curve. A rope with no slack is just a taut string.

Related: [[render-pipeline]], [[game-loop]]
```

IMPORTANT: in the actual file, the code-fence lines must be real triple
backticks, not the escaped `\`\`\`` shown above (escaped only so this plan
renders).

- [ ] **Step 2: Commit**

```bash
git add docs/engine/rope-physics.md
git commit -m "Add rope-physics concept note"
```
(plus the `Co-Authored-By` trailer)

---

## Done

The engine now has a general Verlet physics core (`VerletPoint`,
`DistanceConstraint`) reusable for cloth, chains, and soft bodies; a `Rope`
helper; and debug-line rendering. The Strandbound game has a rope that hangs
and swings as the player drags it. M4 (tie / cut / pull) builds on this and
gets its own spec and plan.
