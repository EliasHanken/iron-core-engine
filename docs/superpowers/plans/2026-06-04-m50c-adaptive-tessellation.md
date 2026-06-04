# M50c Distance-Adaptive Tessellation LOD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the M50b tessellation factor distance-adaptive (per-edge screen-space LOD), with a receding ground-grid demo + fixed/adaptive toggle showing the near-dense/far-coarse gradient.

**Architecture:** The `tesc` reads a mode flag + target/max from spare `LitUbo` slots; in adaptive mode it sets each `gl_TessLevelOuter[i]` from the NDC length of that edge (crack-free — a pure function of the two shared vertices), capped by max, with a behind-near-plane guard. A CPU port locks the math. Config rides in `lightCounts.y/.z` + `probeBoxMin.w` (no `LitUbo` growth). The demo adds a large receding tessellated ground grid + an F3 toggle.

**Tech Stack:** C++17, Vulkan, glslang, ImGui, CTest. Branched from `main` (`ad8523a`) — has the full M50b tessellation pipeline.

---

## Background: exact code this plan builds on

- **`standardLitTescSource()`** (`StandardLitShader.h:383-416`) currently sets a uniform factor:
  ```glsl
  void main() {
      if (gl_InvocationID == 0) {
          float f = max(u.probeBoxMin.w, 1.0);
          gl_TessLevelInner[0] = f;
          gl_TessLevelOuter[0] = f; gl_TessLevelOuter[1] = f; gl_TessLevelOuter[2] = f;
      }
      tcPos[gl_InvocationID] = cpPos[gl_InvocationID]; /* ...normal/uv/tangent */
  }
  ```
  It has the full `LitUbo` block (set=0, binding=0) and `cpPos[]` (local-space control points). `u.mvp` = proj·view·model.
- **`LitUbo`** spare slots: `lightCounts` is `x=pointLightCount, y/z/w padding` (set in `recordSceneDraw` at `:639` as `Vec4{count, 0,0,0}`). `probeBoxMin.w` = M50b tess factor.
- **recordSceneDraw tessellated block** (`VulkanRenderer.cpp:685-692`):
  ```cpp
  const bool tessellated = (sh.tescModule != VK_NULL_HANDLE);
  if (tessellated) {
      ubo.reflectionParams.w = call.material.heightScale;  // displacement
      ubo.probeBoxMin.w      = pendingTessFactor_;          // factor
      ubo.baseColorFactor.w  = 0.0f;                        // POM off
  }
  ```
  (`recordSkinnedDraw` has its own `ubo.lightCounts` set ~`:893` but skinned isn't tessellated — leave it.)
- **Renderer pending pattern:** `pendingTessFactor_ = 16.0f` + `setTessellationFactor` (M50b). `setWireframe`/`pendingWireframe_`. New setters mirror these. New pure-virtuals need OpenGL/Mock stubs.
- **Mesh builders** (`engine/scene/Mesh.h`): `void appendQuad(MeshData& out, Vec3 center, Vec2 size, Vec3 normal);` (+ appendBox/appendTube). `MeshData` holds `vertices` + `indices`; `Vertex = {Vec3 position; Vec3 normal; Vec2 uv; Vec3 tangent;}`.
- **Sandbox** (`games/11-sandbox/main.cpp`): M50b demo + F2 wireframe (`input.keyPressed(GLFW_KEY_F2)`) + `tessFactor` slider + `setTessellationFactor`.

## File structure

**Create:**
- `engine/render/TessLod.h` — CPU port of the per-edge factor (header-only, lockstep with the tesc).
- `tests/test_tess_lod.cpp` — unit test.

**Modify:**
- `engine/render/StandardLitShader.h` — adaptive `standardLitTescSource()`.
- `engine/render/Renderer.h` + `VulkanRenderer.{h,cpp}` — `setTessellationMode`/`setTessellationTargetEdge` + pending state + UBO writes; OpenGL/Mock stubs.
- `engine/scene/Mesh.h` / `Mesh.cpp` — `appendGrid`.
- `games/11-sandbox/main.cpp` — receding ground + F3 toggle + sliders.
- `tests/CMakeLists.txt` — register `test_tess_lod`.

---

## Task 1: CPU port of the edge-factor math + unit test (TDD)

**Files:** Create `engine/render/TessLod.h`, `tests/test_tess_lod.cpp`; modify `tests/CMakeLists.txt`.

- [ ] **Step 1: Write the failing test** (`tests/test_tess_lod.cpp`)

```cpp
#include "render/TessLod.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace iron;

int main() {
    const float target = 0.1f;   // NDC edge length that maps to factor 1
    const float maxF   = 64.0f;

    // Both endpoints behind the near plane (w<=0) => clamp to max (no collapse).
    assert(tessEdgeFactor(Vec4{0, 0, -1, -0.5f}, Vec4{0, 0, -1, -0.5f}, target, maxF) == maxF);

    // A short on-screen edge (NDC length < target) => factor clamps to 1.
    // clip = (x,y,z,w); ndc = xy/w. Two points 0.02 apart in NDC at w=1.
    float fShort = tessEdgeFactor(Vec4{0.0f, 0, 0, 1}, Vec4{0.02f, 0, 0, 1}, target, maxF);
    assert(std::fabs(fShort - 1.0f) < 1e-4f);

    // A long on-screen edge (NDC length 0.8 = 8*target) => factor 8.
    float fLong = tessEdgeFactor(Vec4{-0.4f, 0, 0, 1}, Vec4{0.4f, 0, 0, 1}, target, maxF);
    assert(std::fabs(fLong - 8.0f) < 1e-3f);

    // Monotonic: a nearer edge (smaller w => larger NDC span) tessellates more than a far one.
    float fNear = tessEdgeFactor(Vec4{-0.1f, 0, 0, 1.0f}, Vec4{0.1f, 0, 0, 1.0f}, target, maxF);
    float fFar  = tessEdgeFactor(Vec4{-0.1f, 0, 0, 5.0f}, Vec4{0.1f, 0, 0, 5.0f}, target, maxF);
    assert(fNear > fFar);

    // Never exceeds maxF.
    assert(tessEdgeFactor(Vec4{-1.0f, 0, 0, 1}, Vec4{1.0f, 0, 0, 1}, target, /*maxF=*/4.0f) == 4.0f);

    std::printf("test_tess_lod: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails (header missing)**

Run: `cmake --build build-vk --target test_tess_lod`
Expected: FAIL — `render/TessLod.h` not found.

- [ ] **Step 3: Write `engine/render/TessLod.h`**

```cpp
#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the per-edge tessellation factor in standardLitTescSource().
// Keep in lockstep with the GLSL. Given two clip-space endpoints of a patch
// edge, returns the tessellation level: clamp(ndcEdgeLength / targetEdge, 1, maxF).
// If either endpoint is behind the near plane (w <= 0), returns maxF so patches
// straddling the camera don't collapse.
inline float tessEdgeFactor(Vec4 clipA, Vec4 clipB, float targetEdge, float maxFactor) {
    if (clipA.w <= 0.0f || clipB.w <= 0.0f) return maxFactor;
    const Vec2 a{clipA.x / clipA.w, clipA.y / clipA.w};
    const Vec2 b{clipB.x / clipB.w, clipB.y / clipB.w};
    const float ndcLen = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    const float t = (targetEdge < 1e-4f) ? 1e-4f : targetEdge;
    return std::clamp(ndcLen / t, 1.0f, maxFactor);
}

}  // namespace iron
```
NOTE: confirm `Vec4`/`Vec2` field names (`.x/.y/.w`) in `engine/math/Vec.h`.

- [ ] **Step 4: Register the test** in `tests/CMakeLists.txt` (match the `iron_add_test(test_parallax test_parallax.cpp)` style):
```cmake
iron_add_test(test_tess_lod test_tess_lod.cpp)
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build-vk --target test_tess_lod && ctest --test-dir build-vk -C Debug -R test_tess_lod --output-on-failure`
Expected: PASS — `test_tess_lod: OK`.

- [ ] **Step 6: Commit**

```bash
git add engine/render/TessLod.h tests/test_tess_lod.cpp tests/CMakeLists.txt
git commit -m "M50c: CPU port of adaptive tessellation edge-factor + unit test"
```

---

## Task 2: Adaptive tessellation-control shader

**Files:** Modify `engine/render/StandardLitShader.h` (`standardLitTescSource`).

- [ ] **Step 1: Replace the tesc body with mode + per-edge adaptive factors**

Replace the `main()` of `standardLitTescSource()` (and add the `edgeFactor` helper above it) — keep the existing ins/outs + LitUbo block unchanged:
```glsl
// M50c — per-edge factor from clip-space NDC length (crack-free: pure function of
// the two shared vertices). Behind near plane (w<=0) => max (no collapse).
float edgeFactor(vec4 ca, vec4 cb, float target, float maxF) {
    if (ca.w <= 0.0 || cb.w <= 0.0) return maxF;
    vec2 a = ca.xy / ca.w;
    vec2 b = cb.xy / cb.w;
    return clamp(length(a - b) / max(target, 1e-4), 1.0, maxF);
}
void main() {
    if (gl_InvocationID == 0) {
        float mode   = u.lightCounts.y;             // M50c: 0=fixed, 1=adaptive
        float target = u.lightCounts.z;             // M50c: adaptive target NDC edge length
        float maxF   = max(u.probeBoxMin.w, 1.0);   // fixed factor / adaptive max
        if (mode < 0.5) {
            gl_TessLevelInner[0] = maxF;
            gl_TessLevelOuter[0] = maxF;
            gl_TessLevelOuter[1] = maxF;
            gl_TessLevelOuter[2] = maxF;
        } else {
            // Project the (undisplaced) local control points to clip space.
            vec4 c0 = u.mvp * vec4(cpPos[0], 1.0);
            vec4 c1 = u.mvp * vec4(cpPos[1], 1.0);
            vec4 c2 = u.mvp * vec4(cpPos[2], 1.0);
            // gl_TessLevelOuter[i] = the edge OPPOSITE control point i.
            float e0 = edgeFactor(c1, c2, target, maxF);   // edge 1-2
            float e1 = edgeFactor(c2, c0, target, maxF);   // edge 2-0
            float e2 = edgeFactor(c0, c1, target, maxF);   // edge 0-1
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = max(e0, max(e1, e2));
        }
    }
    tcPos[gl_InvocationID]     = cpPos[gl_InvocationID];
    tcNormal[gl_InvocationID]  = cpNormal[gl_InvocationID];
    tcUV[gl_InvocationID]      = cpUV[gl_InvocationID];
    tcTangent[gl_InvocationID] = cpTangent[gl_InvocationID];
}
```
This matches `TessLod.h`'s `tessEdgeFactor` (same NDC-length / target / clamp / behind-cam logic). The fixed path (mode 0) reproduces M50b exactly.

- [ ] **Step 2: Build engine + sandbox (glslang compiles the tesc)**

Run: `cmake --build build-vk --target ironcore sandbox`
Expected: PASS — glslang compiles the updated tesc. (Behavior is still fixed/uniform at runtime until the renderer sets mode=1 — `lightCounts.y` defaults 0.)

- [ ] **Step 3: Commit**

```bash
git add engine/render/StandardLitShader.h
git commit -m "M50c: adaptive per-edge tessellation factors in the tesc (mode + NDC edge length)"
```

---

## Task 3: Renderer mode + target-edge config

**Files:** Modify `engine/render/Renderer.h`, `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`, OpenGL + Mock renderers.

- [ ] **Step 1: Interface + stubs**

In `Renderer.h` (near `setTessellationFactor`):
```cpp
    // M50c — true = distance-adaptive (screen-space) tessellation; false = fixed/uniform.
    virtual void setTessellationMode(bool adaptive) = 0;
    // M50c — adaptive target edge length in NDC units (smaller = denser). ~0.08 default.
    virtual void setTessellationTargetEdge(float ndc) = 0;
```
`VulkanRenderer.h`: members `bool pendingTessAdaptive_ = false;` + `float pendingTessTargetEdge_ = 0.08f;`, and overrides:
```cpp
    void setTessellationMode(bool adaptive) override { pendingTessAdaptive_ = adaptive; }
    void setTessellationTargetEdge(float ndc) override { pendingTessTargetEdge_ = ndc; }
```
OpenGL renderer + MockRenderer: empty stubs (grep all Renderer subclasses; none abstract).

- [ ] **Step 2: Write mode + target into the UBO for tess draws**

In `recordSceneDraw`, extend the existing `if (tessellated)` block (`VulkanRenderer.cpp:688-692`) to also set the mode + target (the `lightCounts.x` point-light count is preserved; we only touch .y/.z):
```cpp
    if (tessellated) {
        ubo.reflectionParams.w = call.material.heightScale;  // displacement amount
        ubo.probeBoxMin.w      = pendingTessFactor_;          // fixed factor / adaptive max
        ubo.baseColorFactor.w  = 0.0f;                        // POM off
        ubo.lightCounts.y      = pendingTessAdaptive_ ? 1.0f : 0.0f;  // M50c mode
        ubo.lightCounts.z      = pendingTessTargetEdge_;              // M50c target NDC edge
    }
```
(`ubo.lightCounts` was set earlier in the function with `.x`=count, `.y/.z/.w`=0; this override only runs for tess draws and only changes .y/.z. Confirm this block is after that initial `lightCounts` assignment — it is, per M50b ordering.)

- [ ] **Step 3: Build all + tests**

Run: `cmake --build build-vk && ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: PASS (no abstract subclass; existing behavior unchanged — default mode fixed).

- [ ] **Step 4: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp <opengl + mock files>
git commit -m "M50c: setTessellationMode + setTessellationTargetEdge; write mode/target UBO slots for tess draws"
```

---

## Task 4: `appendGrid` mesh builder

**Files:** Modify `engine/scene/Mesh.h`, `engine/scene/Mesh.cpp`. Test: extend `tests/test_mesh.cpp` if it exists, else a small new check.

- [ ] **Step 1: Declare `appendGrid` in `Mesh.h`** (next to `appendQuad`):
```cpp
// Appends a flat XZ grid of `cells`×`cells` quads centered at `center`, spanning
// `size` (x,z), facing +Y. UVs run 0..1 across the whole grid (scale with the
// material's uvScale for tiling); tangent along +X. Produces (cells+1)^2 verts
// and cells*cells*2 triangles — used as a tessellation patch field.
void appendGrid(MeshData& out, Vec3 center, Vec2 size, int cells);
```

- [ ] **Step 2: Implement in `Mesh.cpp`**

```cpp
void appendGrid(MeshData& out, Vec3 center, Vec2 size, int cells) {
    if (cells < 1) cells = 1;
    const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
    const float halfX = size.x * 0.5f;
    const float halfZ = size.y * 0.5f;
    const int verts = cells + 1;
    for (int z = 0; z < verts; ++z) {
        for (int x = 0; x < verts; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(cells);  // 0..1
            const float fz = static_cast<float>(z) / static_cast<float>(cells);
            Vertex v{};
            v.position = Vec3{center.x - halfX + fx * size.x, center.y,
                              center.z - halfZ + fz * size.y};
            v.normal   = Vec3{0.0f, 1.0f, 0.0f};
            v.uv       = Vec2{fx, fz};
            v.tangent  = Vec3{1.0f, 0.0f, 0.0f};
            out.vertices.push_back(v);
        }
    }
    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            const std::uint32_t i0 = base + static_cast<std::uint32_t>(z * verts + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + static_cast<std::uint32_t>(verts);
            const std::uint32_t i3 = i2 + 1;
            // Two triangles per cell, CCW when viewed from +Y above.
            out.indices.push_back(i0); out.indices.push_back(i2); out.indices.push_back(i1);
            out.indices.push_back(i1); out.indices.push_back(i2); out.indices.push_back(i3);
        }
    }
}
```
NOTE: confirm `MeshData` field names (`vertices`/`indices`) + `Vertex` members + `Vec2`/`Vec3` ctors against `Mesh.h` and match `appendQuad`'s exact style (winding, index type). Adjust winding if `appendQuad` uses the opposite (the tess pipeline is double-sided from M50b, so winding isn't critical for visibility, but match the convention).

- [ ] **Step 3: Build + a quick sanity test**

If `tests/test_mesh.cpp` exists, add a case asserting `appendGrid(out, {0,0,0}, {4,4}, 2)` yields `(2+1)^2 = 9` vertices and `2*2*2 = 8` triangles (24 indices). Otherwise rely on the build + the demo. Run: `cmake --build build-vk --target ironcore` (+ the test if added).
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add engine/scene/Mesh.h engine/scene/Mesh.cpp <test file if touched>
git commit -m "M50c: appendGrid mesh builder (NxN quad grid for tessellation patches)"
```

---

## Task 5: Receding ground demo + F3 toggle + sliders (visual gate)

**Files:** Modify `games/11-sandbox/main.cpp`.

- [ ] **Step 1: Build a receding tessellated ground grid**

In the sandbox scene setup (place it as an isolated demo area, like the M50b quads):
1. Build a grid mesh and upload it:
```cpp
    iron::MeshData groundData;
    iron::appendGrid(groundData, iron::Vec3{kGroundCenter}, iron::Vec2{40.0f, 40.0f}, 20);
    const iron::MeshHandle groundMesh = renderer.createMesh(groundData);  // match the real create-mesh call
```
   (Match the actual mesh-upload API the sandbox uses for other meshes — grep `createMesh`/`uploadMesh`.)
2. Submit it each frame with the tess shader + the stone material (albedo/height/normal from M50b), `uvScale` tuned so the stones tile reasonably across 40 units (e.g. 8), `heightScale` ~0.1, only if `tessShader` is valid:
```cpp
    if (tessShader != iron::kInvalidHandle) {
        iron::DrawCall g{};
        g.mesh = groundMesh; g.shader = tessShader;
        g.model = iron::Mat4::identity();
        g.material = pomBase;            // stone albedo/height/normal
        g.material.uvScale = 8.0f;
        g.material.heightScale = 0.1f;
        renderer.submit(g);
    }
```
3. Position the ground so it recedes toward the horizon from the camera's view (e.g. a 40×40 plane in front of/below the camera). Add a "Adaptive Tessellation Ground (M50c)" HUD label or reuse the panel.

- [ ] **Step 2: F3 fixed/adaptive toggle + sliders + startup mode**

Add state `bool tessAdaptive = true;` and `static float tessTarget = 0.08f;`. In the F-key block:
```cpp
        if (input.keyPressed(GLFW_KEY_F3)) {
            tessAdaptive = !tessAdaptive;
            renderer.setTessellationMode(tessAdaptive);
        }
```
In the tessellation ImGui panel (next to the M50b factor slider):
```cpp
        if (ImGui::Checkbox("Adaptive tessellation (F3)", &tessAdaptive))
            renderer.setTessellationMode(tessAdaptive);
        if (ImGui::SliderFloat("Target edge (NDC)", &tessTarget, 0.02f, 0.4f))
            renderer.setTessellationTargetEdge(tessTarget);
```
At startup (before the loop) call `renderer.setTessellationMode(tessAdaptive);` and `renderer.setTessellationTargetEdge(tessTarget);` so the initial state is applied.

- [ ] **Step 3: Build + run the visual gate**

Run: `cmake --build build-vk --target sandbox` then run `build-vk\games\11-sandbox\Debug\sandbox.exe`.
Expected (visual gate — confirm with user):
- The receding ground shows displaced stone relief; with **F2 wireframe** ON and **adaptive** mode, triangle density is **high near the camera and progressively coarser toward the horizon**.
- **F3** flips to fixed mode: uniform density everywhere (visibly wasteful far away / sparse near) — toggling redistributes the triangles.
- The **target-edge slider** changes overall density; the **factor slider** caps it.
- **No cracks/seams** between grid patches at LOD boundaries.
- The M50b 3-way quads still render correctly.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M50c: receding tessellated ground demo + F3 adaptive toggle + target-edge slider (visual gate)"
```

---

## Self-review notes (spec coverage)

- Screen-space NDC adaptive factor (crack-free, behind-cam guard) → Tasks 1 (CPU) + 2 (GLSL). Mode flag (fixed/adaptive) → Tasks 2 + 3. Config in spare slots (lightCounts.y/.z + probeBoxMin.w, no UBO growth) → Tasks 2 + 3. Renderer setters → Task 3. Receding-grid demo + F3 toggle + sliders → Tasks 4 + 5. CPU-port test → Task 1. All spec sections covered.
- Out-of-scope (fractional smoothing, frustum cull, arbitrary-mesh displacement) → no task. Correct.

## Risks / verification reminders

- **GLSL↔CPU lockstep:** the tesc `edgeFactor` must match `TessLod.h` `tessEdgeFactor` (NDC length, `clamp(.../target, 1, max)`, behind-cam → max). Same convention; change both together.
- **Edge↔outer mapping:** `gl_TessLevelOuter[i]` = edge opposite control point i (outer[0]→edge 1-2, outer[1]→edge 2-0, outer[2]→edge 0-1). Getting this wrong shows as cracks/asymmetric density. Verify at the visual gate (no cracks).
- **`lightCounts` not clobbered:** the tess block only sets `.y/.z`; `.x` (point-light count, set earlier) must remain. Confirm ordering (tess override after the initial `lightCounts` assignment).
- **`appendGrid` winding / MeshData API:** match `appendQuad`'s exact `MeshData`/`Vertex`/index conventions; the tess pipeline is double-sided (M50b) so culling won't hide winding mistakes — but keep it consistent.
- **All Renderer subclasses** stub `setTessellationMode` + `setTessellationTargetEdge` (build all targets — [[verify-clean-build-before-ci]]).
- **Visual gate owns correctness:** the LOD gradient + no-cracks + toggle redistribution are confirmed by running the sandbox (not headless-testable).
- Clean build + full ctest at the end.
