# Strandbound — "Solid Ropes & Anchors" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 1-pixel debug-line ropes and anchors with real geometry — ropes become textured, lit, low-poly tube meshes; anchors become solid lit cubes.

**Architecture:** Builds on the completed M1–M4 engine. Adds `updateMesh` to the renderer (so a mesh can be re-uploaded each frame), and two pure geometry builders — `appendTube` and `appendBox` — to `engine/scene/`. The Strandbound `RopeTool` builds one combined tube mesh for all ropes and one combined cube mesh for all anchors each frame, and draws them through the existing lit render path.

**Tech Stack:** C++23, CMake, MSVC, OpenGL 3.3 (via the existing RHI). No new third-party dependencies. One downloaded CC0 texture asset.

**Conventions:**
- Namespace `iron` for engine code. Engine headers included relative to `engine/`. `Mat4` column-major.
- Build: `cmake -S . -B build` then `cmake --build build`.
- Tests (MSVC multi-config): `ctest --test-dir build -C Debug --output-on-failure`.
- Commit after every task; commit messages end with the `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- Spec: `docs/superpowers/specs/2026-05-21-strandbound-solid-ropes-and-anchors-design.md`.

---

## File Structure

**Created by this plan:**

```
tests/test_mesh_builders.cpp                         appendBox / appendTube unit tests
games/02-strandbound/assets/rope.jpg                 downloaded CC0 rope texture
docs/engine/procedural-meshes.md                     concept note
```

**Modified by this plan:**

```
engine/render/Renderer.h                             updateMesh added to the RHI
engine/render/backends/opengl/GLMesh.h/.cpp           dynamic buffers + update()
engine/render/backends/opengl/OpenGLRenderer.h/.cpp   updateMesh implementation
engine/scene/Mesh.h, Mesh.cpp                         appendBox + appendTube (makeCube delegates to appendBox)
tests/CMakeLists.txt                                  register test_mesh_builders
games/02-strandbound/RopeTool.h, .cpp                 textured tube + cube rendering
games/02-strandbound/main.cpp                         pass renderer + shader to RopeTool
```

---

## Task 1: Dynamic meshes (`updateMesh`)

Make `GLMesh` re-uploadable and expose `updateMesh` on the renderer.

**Files:**
- Modify: `engine/render/backends/opengl/GLMesh.h`
- Modify: `engine/render/backends/opengl/GLMesh.cpp`
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp`

Read each file before editing.

- [ ] **Step 1: Declare `update` in `GLMesh.h`**

In `engine/render/backends/opengl/GLMesh.h`, add a public method declaration after `void draw() const;`:

```cpp
    void draw() const;

    // Re-upload the mesh's geometry. The vertex layout must be unchanged.
    void update(const MeshData& data);
```

- [ ] **Step 2: Make `GLMesh` buffers dynamic and add `update` in `GLMesh.cpp`**

In `engine/render/backends/opengl/GLMesh.cpp`, in the constructor, change **both** `glBufferData` calls from `GL_STATIC_DRAW` to `GL_DYNAMIC_DRAW` (the `GL_ARRAY_BUFFER` upload and the `GL_ELEMENT_ARRAY_BUFFER` upload).

Then add this definition after the constructor (before `release()`):

```cpp
void GLMesh::update(const MeshData& data) {
    indexCount_ = static_cast<std::int32_t>(data.indices.size());

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(), GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}
```

- [ ] **Step 3: Add `updateMesh` to the RHI interface `Renderer.h`**

In `engine/render/Renderer.h`, add a pure-virtual method right after the `createMesh` declaration:

```cpp
    virtual MeshHandle createMesh(const MeshData& data) = 0;
    // Replace the geometry of an existing mesh (for meshes rebuilt per frame).
    virtual void updateMesh(MeshHandle mesh, const MeshData& data) = 0;
```

- [ ] **Step 4: Declare the override in `OpenGLRenderer.h`**

In `engine/render/backends/opengl/OpenGLRenderer.h`, add after the `createMesh` override declaration:

```cpp
    MeshHandle createMesh(const MeshData& data) override;
    void updateMesh(MeshHandle mesh, const MeshData& data) override;
```

- [ ] **Step 5: Define `updateMesh` in `OpenGLRenderer.cpp`**

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, add after the `createMesh` definition:

```cpp
void OpenGLRenderer::updateMesh(MeshHandle mesh, const MeshData& data) {
    // Handles are (index + 1); reject anything out of range.
    if (mesh == kInvalidHandle || mesh > meshes_.size()) {
        Log::warn("OpenGLRenderer::updateMesh: mesh handle out of range");
        return;
    }
    meshes_[mesh - 1]->update(data);
}
```

- [ ] **Step 6: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean — `ironcore`, both games, all test executables. `OpenGLRenderer` is still concrete (the new pure-virtual is overridden).

- [ ] **Step 7: Commit**

```bash
git add engine
git commit -m "Add updateMesh: re-uploadable dynamic meshes"
```
(plus the `Co-Authored-By` trailer)

---

## Task 2: `appendBox` geometry builder

Add an `appendBox` builder and rewrite `makeCube` to delegate to it (so the cube geometry stays identical, DRY).

**Files:**
- Create: `tests/test_mesh_builders.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `engine/scene/Mesh.h`
- Modify: `engine/scene/Mesh.cpp`

- [ ] **Step 1: Write the failing test `tests/test_mesh_builders.cpp`**

```cpp
#include "test_framework.h"
#include "math/Vec.h"
#include "scene/Mesh.h"

#include <cstdint>

using namespace iron;

int main() {
    // appendBox into an empty mesh yields 24 vertices and 36 indices.
    {
        MeshData m;
        appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f});
        CHECK(m.vertices.size() == 24);
        CHECK(m.indices.size() == 36);
    }

    // Appending a second box accumulates; its indices reference its own
    // vertices (offset past the first box) and stay in range.
    {
        MeshData m;
        appendBox(m, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
        appendBox(m, Vec3{10.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
        CHECK(m.vertices.size() == 48);
        CHECK(m.indices.size() == 72);
        std::uint32_t maxIndex = 0;
        for (std::uint32_t i : m.indices) {
            if (i > maxIndex) maxIndex = i;
        }
        CHECK(maxIndex == 47);
    }

    // A box spans center +/- size/2.
    {
        MeshData m;
        appendBox(m, Vec3{5.0f, 0.0f, 0.0f}, Vec3{4.0f, 2.0f, 2.0f});
        float minX = 1e30f;
        float maxX = -1e30f;
        for (const Vertex& v : m.vertices) {
            if (v.position.x < minX) minX = v.position.x;
            if (v.position.x > maxX) maxX = v.position.x;
        }
        CHECK_NEAR(minX, 3.0f);
        CHECK_NEAR(maxX, 7.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

Add after the existing `iron_add_test(test_ray test_ray.cpp)` line:

```cmake
iron_add_test(test_mesh_builders test_mesh_builders.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `appendBox` does not exist.

- [ ] **Step 4: Declare `appendBox` in `engine/scene/Mesh.h`**

In `engine/scene/Mesh.h`, add after the `makeCube()` declaration:

```cpp
MeshData makeCube();

// Appends a box (a cuboid) to `out`: 24 vertices (per-face normals + UVs) and
// 36 indices, centered at `center` with full extents `size`. Indices are
// offset so the box references its own vertices when appended to a non-empty
// MeshData.
void appendBox(MeshData& out, Vec3 center, Vec3 size);
```

- [ ] **Step 5: Rewrite `engine/scene/Mesh.cpp` with `appendBox` + a delegating `makeCube`**

Replace the entire contents of `engine/scene/Mesh.cpp` with:

```cpp
#include "scene/Mesh.h"

#include <cstdint>

namespace iron {

void appendBox(MeshData& out, Vec3 center, Vec3 size) {
    // Six faces, each a quad of 4 vertices with a shared outward normal.
    // Corner components are +/-0.5 (a unit cube); scaled by `size` and
    // shifted by `center` they span center +/- size/2. Winding is
    // counter-clockwise seen from outside.
    struct Face {
        Vec3 normal;
        Vec3 corners[4];
    };

    const Face faces[6] = {
        {{ 1, 0, 0}, {{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f}}},
        {{-1, 0, 0}, {{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 1, 0}, {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}}},
        {{ 0,-1, 0}, {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0, 0,-1}, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}}},
    };

    const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (int i = 0; i < 4; ++i) {
            const Vec3 c = face.corners[i];
            const Vec3 position{
                center.x + c.x * size.x,
                center.y + c.y * size.y,
                center.z + c.z * size.z,
            };
            out.vertices.push_back(Vertex{position, face.normal, uvs[i]});
        }
        // Two triangles per quad: (0,1,2) and (0,2,3).
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
    }
}

// A unit cube centered at the origin (side length 1) — built from appendBox.
MeshData makeCube() {
    MeshData data;
    appendBox(data, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    return data;
}

} // namespace iron
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 8 tests pass, including `test_mesh_builders`. (The spinning-cube and Strandbound games still render their cubes — `makeCube` is unchanged in output.)

- [ ] **Step 7: Commit**

```bash
git add engine/scene/Mesh.h engine/scene/Mesh.cpp tests/test_mesh_builders.cpp tests/CMakeLists.txt
git commit -m "Add appendBox; makeCube delegates to it"
```
(plus the `Co-Authored-By` trailer)

---

## Task 3: `appendTube` geometry builder

**Files:**
- Modify: `engine/scene/Mesh.h`
- Modify: `engine/scene/Mesh.cpp`
- Modify: `tests/test_mesh_builders.cpp`

- [ ] **Step 1: Add `appendTube` tests to `tests/test_mesh_builders.cpp`**

Add `#include <cmath>` and `#include <vector>` to the includes. Then add these three test blocks immediately before the `return iron_test_result();` line:

```cpp
    // appendTube over 3 points with 6 sides: 3*6 = 18 vertices,
    // (3-1)*6*6 = 72 indices.
    {
        MeshData m;
        std::vector<Vec3> pts = {
            Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f},
            Vec3{0.0f, 0.0f, -2.0f},
        };
        appendTube(m, pts, 0.5f, 6);
        CHECK(m.vertices.size() == 18);
        CHECK(m.indices.size() == 72);
    }

    // Tube ring vertices sit at `radius` from the polyline; normals are unit
    // length and point radially outward.
    {
        MeshData m;
        std::vector<Vec3> pts = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -4.0f}};
        appendTube(m, pts, 2.0f, 8);
        for (const Vertex& v : m.vertices) {
            // The polyline runs along Z, so distance from the Z axis is the radius.
            CHECK_NEAR(std::sqrt(v.position.x * v.position.x
                               + v.position.y * v.position.y), 2.0f);
            CHECK_NEAR(std::sqrt(v.normal.x * v.normal.x
                               + v.normal.y * v.normal.y
                               + v.normal.z * v.normal.z), 1.0f);
        }
    }

    // Degenerate input (fewer than 2 points) appends nothing.
    {
        MeshData m;
        std::vector<Vec3> one = {Vec3{0.0f, 0.0f, 0.0f}};
        appendTube(m, one, 1.0f, 6);
        CHECK(m.vertices.empty());
        CHECK(m.indices.empty());
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: build FAILS — `appendTube` does not exist.

- [ ] **Step 3: Declare `appendTube` in `engine/scene/Mesh.h`**

In `engine/scene/Mesh.h`, add after the `appendBox` declaration:

```cpp
// Appends a low-poly tube around the polyline `points` to `out`. Each point
// gets a ring of `sides` vertices at distance `radius`, with outward normals
// and UVs (U around the ring, V tiling along the rope's length); consecutive
// rings are stitched into triangles. Does nothing if there are fewer than 2
// points or fewer than 3 sides.
void appendTube(MeshData& out, const std::vector<Vec3>& points, float radius,
                int sides);
```

- [ ] **Step 4: Define `appendTube` in `engine/scene/Mesh.cpp`**

Add `#include "math/Vec.h"` is already pulled in via `Mesh.h`; add `#include <cmath>` at the top of `Mesh.cpp` (after `#include <cstdint>`). Then add this function inside `namespace iron`, after `appendBox`:

```cpp
void appendTube(MeshData& out, const std::vector<Vec3>& points, float radius,
                int sides) {
    const int pointCount = static_cast<int>(points.size());
    if (pointCount < 2 || sides < 3) {
        return;
    }

    constexpr float kTwoPi = 6.28318530717958647692f;
    const auto base = static_cast<std::uint32_t>(out.vertices.size());

    // --- ring vertices ---
    float vCoord = 0.0f;
    for (int i = 0; i < pointCount; ++i) {
        // Local rope direction (forward difference; backward at the last point).
        Vec3 dir = (i + 1 < pointCount) ? points[i + 1] - points[i]
                                        : points[i] - points[i - 1];
        const float dirLen = length(dir);
        dir = (dirLen > 1e-6f) ? dir * (1.0f / dirLen) : Vec3{0.0f, 0.0f, 1.0f};

        // A perpendicular frame. Use world up unless the rope is near-vertical.
        const Vec3 up = (std::fabs(dir.y) > 0.99f) ? Vec3{1.0f, 0.0f, 0.0f}
                                                   : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 right = normalize(cross(dir, up));
        const Vec3 ringUp = normalize(cross(right, dir));

        // V advances with arc length so the texture tiles down the rope.
        if (i > 0) {
            vCoord += length(points[i] - points[i - 1]) / (2.0f * radius);
        }

        for (int s = 0; s < sides; ++s) {
            const float angle = kTwoPi * static_cast<float>(s)
                                       / static_cast<float>(sides);
            const Vec3 offset = right * (std::cos(angle) * radius)
                              + ringUp * (std::sin(angle) * radius);
            Vertex vert;
            vert.position = points[i] + offset;
            vert.normal = normalize(offset);  // radially outward
            vert.uv = Vec2{static_cast<float>(s) / static_cast<float>(sides),
                           vCoord};
            out.vertices.push_back(vert);
        }
    }

    // --- stitch consecutive rings (CCW seen from outside) ---
    for (int i = 0; i + 1 < pointCount; ++i) {
        const auto ring0 = base + static_cast<std::uint32_t>(i * sides);
        const auto ring1 = base + static_cast<std::uint32_t>((i + 1) * sides);
        for (int s = 0; s < sides; ++s) {
            const auto s0 = static_cast<std::uint32_t>(s);
            const auto s1 = static_cast<std::uint32_t>((s + 1) % sides);
            out.indices.push_back(ring0 + s0);
            out.indices.push_back(ring1 + s0);
            out.indices.push_back(ring1 + s1);
            out.indices.push_back(ring0 + s0);
            out.indices.push_back(ring1 + s1);
            out.indices.push_back(ring0 + s1);
        }
    }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 8 tests pass; `test_mesh_builders` now covers boxes and tubes.

- [ ] **Step 6: Commit**

```bash
git add engine/scene/Mesh.h engine/scene/Mesh.cpp tests/test_mesh_builders.cpp
git commit -m "Add appendTube geometry builder with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 4: Download the rope texture

**Files:**
- Create: `games/02-strandbound/assets/rope.jpg`

- [ ] **Step 1: Download and extract the rope texture**

The texture is `Rope002` from ambientCG (CC0 / public domain). Run (PowerShell):

```powershell
$tmp = Join-Path $env:TEMP "ironcore_rope_dl"
New-Item -ItemType Directory -Force $tmp | Out-Null
Invoke-WebRequest -Uri "https://ambientcg.com/get?file=Rope002_1K-JPG.zip" -OutFile "$tmp\rope.zip"
Expand-Archive -Force "$tmp\rope.zip" -DestinationPath "$tmp\rope"
Copy-Item "$tmp\rope\Rope002_1K-JPG_Color.jpg" "games\02-strandbound\assets\rope.jpg"
Remove-Item -Recurse -Force $tmp
```

Expected: `games/02-strandbound/assets/rope.jpg` exists — a ~500 KB, 1024×512 JPEG (the rope colour/albedo map).

If the download fails (network), STOP and report BLOCKED — do not fabricate the asset.

- [ ] **Step 2: Verify the file**

Confirm `games/02-strandbound/assets/rope.jpg` exists and is a non-empty JPEG (~500 KB). Confirm `.gitignore` does not exclude `.jpg` (the spinning-cube and strandbound `crate.jpg` assets are already tracked, so it does not).

- [ ] **Step 3: Commit**

```bash
git add games/02-strandbound/assets/rope.jpg
git commit -m "Add CC0 rope texture asset (ambientCG Rope002)"
```
(plus the `Co-Authored-By` trailer)

---

## Task 5: RopeTool renders solid textured geometry

Rewrite `RopeTool` to draw ropes as a combined textured tube mesh and anchors as a combined cube mesh, and wire the new constructor signature in `main.cpp`.

**Files:**
- Modify: `games/02-strandbound/RopeTool.h`
- Modify: `games/02-strandbound/RopeTool.cpp`
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Replace `games/02-strandbound/RopeTool.h` entirely with this**

```cpp
#pragma once

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"
#include "render/Renderer.h"

#include <vector>

// The Strandbound rope tool: the player places anchor points on world
// surfaces and ties / cuts ropes between them. Game-specific interaction —
// it lives with the game, not the engine.
class RopeTool {
public:
    // `colliders` are the static world boxes the aim ray is tested against
    // when placing an anchor. `renderer` is used to create the tool's own GPU
    // resources (rope/anchor meshes and textures); `litShader` is the shader
    // the ropes and anchors are drawn with.
    RopeTool(std::vector<iron::Aabb> colliders, iron::Renderer& renderer,
             iron::ShaderHandle litShader);

    // Advance one fixed step. `aim` is the player's aim ray; `playerPos` is
    // the player's feet position. The three flags are this step's input edges.
    void update(const iron::Ray& aim, iron::Vec3 playerPos,
                bool placePressed, bool tiePressed, bool cutPressed,
                float dt);

    // Rebuild and draw the rope/anchor meshes, and queue the aim marker.
    // Call between submitting the scene and flushDebugLines.
    void draw(iron::Renderer& renderer, const iron::Mat4& view,
              const iron::Mat4& projection) const;

private:
    enum class AimKind { None, Surface, Anchor, Rope };

    int pickAnchor(const iron::Ray& aim) const;
    int pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const;
    bool pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const;
    void refreshAimTarget(const iron::Ray& aim);

    std::vector<iron::Aabb> colliders_;
    std::vector<iron::Vec3> anchors_;
    std::vector<iron::Rope> ropes_;

    int tyingFromAnchor_ = -1;        // anchor index being tied from, or -1
    iron::Vec3 playerPos_{};          // cached, for drawing the tying guide

    AimKind aimKind_ = AimKind::None;
    iron::Vec3 aimPoint_{};

    iron::ShaderHandle litShader_ = iron::kInvalidHandle;
    iron::TextureHandle ropeTexture_ = iron::kInvalidHandle;
    iron::TextureHandle anchorTexture_ = iron::kInvalidHandle;
    iron::MeshHandle ropesMesh_ = iron::kInvalidHandle;
    iron::MeshHandle anchorsMesh_ = iron::kInvalidHandle;
};
```

- [ ] **Step 2: Replace `games/02-strandbound/RopeTool.cpp` entirely with this**

```cpp
#include "RopeTool.h"

#include "core/Platform.h"
#include "physics/VerletPoint.h"
#include "scene/Mesh.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace {
constexpr float kAnchorPickRadius = 0.5f;   // anchors picked as spheres
constexpr float kRopePickRadius = 0.3f;     // rope points picked as spheres
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;       // rope length vs. anchor span
constexpr float kMarkerSize = 0.25f;        // aim-marker cross half-length
constexpr float kMinPlaceDistance = 0.1f;   // ignore surface hits at the eye

constexpr float kRopeRadius = 0.08f;        // visual rope thickness
constexpr int kRopeSides = 6;               // low-poly tube cross-section
constexpr float kAnchorCubeSize = 0.4f;     // anchor marker cube side
}  // namespace

RopeTool::RopeTool(std::vector<iron::Aabb> colliders, iron::Renderer& renderer,
                   iron::ShaderHandle litShader)
    : colliders_(std::move(colliders)), litShader_(litShader) {
    // The rope texture ships next to the executable; anchors use a 1x1 solid
    // yellow texture (the lit shader is texture * lighting, so a flat colour
    // tints the whole cube).
    ropeTexture_ = renderer.loadTexture(iron::executableDir() + "/assets/rope.jpg");
    const unsigned char yellow[4] = {235, 200, 40, 255};
    anchorTexture_ = renderer.createTexture(1, 1, yellow);

    // Two meshes, created empty and refreshed every frame in draw().
    ropesMesh_ = renderer.createMesh(iron::MeshData{});
    anchorsMesh_ = renderer.createMesh(iron::MeshData{});
}

int RopeTool::pickAnchor(const iron::Ray& aim) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < anchors_.size(); ++i) {
        float t = 0.0f;
        if (iron::intersectRaySphere(aim, anchors_[i], kAnchorPickRadius, t)
                && t < bestT) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int RopeTool::pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < ropes_.size(); ++i) {
        for (const iron::VerletPoint& p : ropes_[i].points()) {
            float t = 0.0f;
            if (iron::intersectRaySphere(aim, p.position, kRopePickRadius, t)
                    && t < bestT) {
                bestT = t;
                best = static_cast<int>(i);
                outPoint = p.position;
            }
        }
    }
    return best;
}

bool RopeTool::pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const {
    float bestT = 1e30f;
    bool found = false;
    for (const iron::Aabb& box : colliders_) {
        float t = 0.0f;
        // Skip hits at ~t=0: those happen when the player's eye is inside a
        // box, and would place an anchor floating at the eye position.
        if (iron::intersectRayAabb(aim, box, t) && t > kMinPlaceDistance
                && t < bestT) {
            bestT = t;
            found = true;
        }
    }
    if (found) {
        outPoint = aim.origin + aim.direction * bestT;
    }
    return found;
}

void RopeTool::refreshAimTarget(const iron::Ray& aim) {
    const int anchor = pickAnchor(aim);
    if (anchor >= 0) {
        aimKind_ = AimKind::Anchor;
        aimPoint_ = anchors_[static_cast<std::size_t>(anchor)];
        return;
    }
    iron::Vec3 ropePoint;
    if (pickRope(aim, ropePoint) >= 0) {
        aimKind_ = AimKind::Rope;
        aimPoint_ = ropePoint;
        return;
    }
    iron::Vec3 surfacePoint;
    if (pickSurface(aim, surfacePoint)) {
        aimKind_ = AimKind::Surface;
        aimPoint_ = surfacePoint;
        return;
    }
    aimKind_ = AimKind::None;
}

void RopeTool::update(const iron::Ray& aim, iron::Vec3 playerPos,
                      bool placePressed, bool tiePressed, bool cutPressed,
                      float dt) {
    playerPos_ = playerPos;

    if (placePressed) {
        iron::Vec3 hit;
        if (pickSurface(aim, hit)) {
            anchors_.push_back(hit);
        }
    }

    if (tiePressed) {
        const int anchor = pickAnchor(aim);
        if (anchor >= 0) {
            if (tyingFromAnchor_ < 0) {
                tyingFromAnchor_ = anchor;
            } else if (anchor != tyingFromAnchor_) {
                const iron::Vec3 a =
                    anchors_[static_cast<std::size_t>(tyingFromAnchor_)];
                const iron::Vec3 b =
                    anchors_[static_cast<std::size_t>(anchor)];
                const float span = iron::length(b - a);
                ropes_.push_back(iron::Rope(a, b, kRopeSegments,
                                            span * kSlackFactor));
                tyingFromAnchor_ = -1;
            }
        }
    }

    if (cutPressed) {
        iron::Vec3 unused;
        const int rope = pickRope(aim, unused);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin()
                         + static_cast<std::ptrdiff_t>(rope));
        }
    }

    // Anchors are static in this milestone — ropes are constructed pinned to
    // their anchor positions and never re-synced.
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }

    refreshAimTarget(aim);
}

void RopeTool::draw(iron::Renderer& renderer, const iron::Mat4& view,
                    const iron::Mat4& projection) const {
    // Rebuild the combined rope tube mesh from every rope's current points.
    iron::MeshData ropeGeometry;
    for (const iron::Rope& r : ropes_) {
        std::vector<iron::Vec3> pts;
        pts.reserve(r.points().size());
        for (const iron::VerletPoint& p : r.points()) {
            pts.push_back(p.position);
        }
        iron::appendTube(ropeGeometry, pts, kRopeRadius, kRopeSides);
    }
    renderer.updateMesh(ropesMesh_, ropeGeometry);

    // Rebuild the combined anchor cube mesh.
    iron::MeshData anchorGeometry;
    const iron::Vec3 anchorSize{kAnchorCubeSize, kAnchorCubeSize,
                                kAnchorCubeSize};
    for (const iron::Vec3& a : anchors_) {
        iron::appendBox(anchorGeometry, a, anchorSize);
    }
    renderer.updateMesh(anchorsMesh_, anchorGeometry);

    // Draw both through the lit render path.
    iron::DrawCall ropeCall;
    ropeCall.mesh = ropesMesh_;
    ropeCall.shader = litShader_;
    ropeCall.texture = ropeTexture_;
    renderer.submit(ropeCall, view, projection);

    iron::DrawCall anchorCall;
    anchorCall.mesh = anchorsMesh_;
    anchorCall.shader = litShader_;
    anchorCall.texture = anchorTexture_;
    renderer.submit(anchorCall, view, projection);

    // While tying: a guide line from the start anchor to the player.
    if (tyingFromAnchor_ >= 0) {
        renderer.drawLine(anchors_[static_cast<std::size_t>(tyingFromAnchor_)],
                          playerPos_ + iron::Vec3{0.0f, 1.0f, 0.0f},
                          iron::Vec3{0.3f, 0.85f, 0.95f});
    }

    // Aim marker: a small cross at the targeted point, coloured by kind.
    if (aimKind_ != AimKind::None) {
        iron::Vec3 c{1.0f, 1.0f, 1.0f};  // Surface -> white
        if (aimKind_ == AimKind::Anchor) {
            c = iron::Vec3{0.95f, 0.8f, 0.2f};
        } else if (aimKind_ == AimKind::Rope) {
            c = iron::Vec3{0.95f, 0.25f, 0.2f};
        }
        const float s = kMarkerSize * 0.7f;
        renderer.drawLine(aimPoint_ - iron::Vec3{s, 0.0f, 0.0f},
                          aimPoint_ + iron::Vec3{s, 0.0f, 0.0f}, c);
        renderer.drawLine(aimPoint_ - iron::Vec3{0.0f, s, 0.0f},
                          aimPoint_ + iron::Vec3{0.0f, s, 0.0f}, c);
        renderer.drawLine(aimPoint_ - iron::Vec3{0.0f, 0.0f, s},
                          aimPoint_ + iron::Vec3{0.0f, 0.0f, s}, c);
    }
}
```

- [ ] **Step 3: Update `games/02-strandbound/main.cpp`**

In `games/02-strandbound/main.cpp`, find the `RopeTool` construction:

```cpp
    RopeTool ropeTool(colliders);
```

Change it to pass the renderer and the lit shader handle:

```cpp
    RopeTool ropeTool(colliders, renderer, shader);
```

Then find the `RopeTool` draw call in the `setRender` lambda:

```cpp
        ropeTool.draw(renderer);
```

Change it to pass the view and projection matrices (both are already in scope in that lambda — `view` is computed from `player.viewMatrix()`, `projection` is the const declared earlier):

```cpp
        ropeTool.draw(renderer, view, projection);
```

- [ ] **Step 4: Build**

Run: `cmake --build build`
Expected: builds clean; `strandbound.exe` is produced.

- [ ] **Step 5: Run — milestone acceptance check**

Run: `build/games/02-strandbound/Debug/strandbound.exe`

Expected — **this is the acceptance check**:
- Ropes are solid, rounded, **textured** tubes that catch the directional
  light and hang in a visible curve — not thin lines.
- Anchors are solid lit yellow cubes, clearly visible.
- Placing (right-click), tying (left-click ×2), and cutting (`C`) all still
  work; a cut rope disappears cleanly.
- The aim-marker cross still tracks the gaze and recolours by target.
- `Escape` quits.

> Visual verification is done by the controller / user, not an implementer
> subagent. If running the game blocks, just confirm it builds and launches
> without an immediate crash.

- [ ] **Step 6: Commit**

```bash
git add games/02-strandbound/RopeTool.h games/02-strandbound/RopeTool.cpp games/02-strandbound/main.cpp
git commit -m "MILESTONE: ropes and anchors as solid textured geometry"
```
(plus the `Co-Authored-By` trailer)

---

## Task 6: Procedural-meshes concept note

**Files:**
- Create: `docs/engine/procedural-meshes.md`

- [ ] **Step 1: Write `docs/engine/procedural-meshes.md`**

Create the file with exactly this content (plain Markdown prose — no code fences):

```
# Procedural Meshes

Not every mesh comes from a file. Some are *built in code* — and some are
rebuilt every frame because the thing they represent keeps moving.

## Geometry builders

`appendBox` and `appendTube` are pure functions that append vertices and
indices to a `MeshData`. "Append" rather than "return" matters: it lets many
shapes accumulate into one mesh. The rope tool builds a single mesh holding
every rope, and another holding every anchor cube, by appending into one
`MeshData` in a loop.

`makeCube` is now just `appendBox` at the origin with side length 1 — one
definition of the cube geometry, reused.

## Tube generation

A tube around a polyline is a stack of rings. At each point, a ring of `sides`
vertices is placed in the plane perpendicular to the local direction; outward
normals make it catch the light as a round surface even at a low side count.
Consecutive rings are stitched into triangle bands. The orientation frame is
computed with a simple reference-up cross product — good enough for a hanging
rope; a twisting tube would want parallel transport.

## Dynamic meshes

A rope deforms every frame as its Verlet simulation moves. A normal mesh is
uploaded to the GPU once. `Renderer::updateMesh` re-uploads a mesh's geometry,
so a mesh handle can be created once and refreshed each frame. The buffers are
marked `GL_DYNAMIC_DRAW` to tell the driver the data changes often.

The rope tool exploits this: one rope mesh and one anchor mesh, rebuilt and
re-uploaded every frame. A cut rope simply is not included in next frame's
rebuild — no per-rope GPU resource to free.

Related: [[rope-physics]], [[render-pipeline]]
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/procedural-meshes.md
git commit -m "Add procedural-meshes concept note"
```
(plus the `Co-Authored-By` trailer)

---

## Done

The engine has dynamic, re-uploadable meshes and two reusable geometry
builders (`appendBox`, `appendTube`). The Strandbound ropes are solid,
textured, lit tubes and the anchors are solid cubes. M5 ("bridge the gap")
builds on this and gets its own spec and plan.
