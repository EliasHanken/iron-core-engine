# M32 — Gizmo Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the five M31 gizmo rough edges — render the gizmo on top of geometry, make clicking a handle reliably grab it, smooth the drag, center the gizmo on the visible mesh, and highlight handles on hover.

**Architecture:** Engine gains a depth-disabled "overlay" line path in `VkDebugLines` exposed as `Renderer::drawLineOverlay` (default forwards to `drawLine`, Vulkan overrides). The editor `Gizmo` takes an explicit world-AABB-center origin, hit-tests handles for hover + click-grab (forgiving tolerance), holds the last axis param on degenerate solves (smooth drag), and draws via the overlay path with the hovered/active handle brightened. The sandbox passes the selected entity's world-AABB center each frame.

**Tech Stack:** C++23, Vulkan 1.3, existing debug-line renderer, CMake. No new third-party deps.

**Spec:** `docs/superpowers/specs/2026-05-29-m32-gizmo-polish-design.md`

**Branch:** `feat/m32-gizmo-polish` (off `main` at the M31 merge `1ca9db7`, spec committed).

---

## Verified ground-truth (match exactly)

```cpp
// engine/render/Renderer.h (abstract): virtual void drawLine(Vec3 a, Vec3 b, Vec3 color) = 0;
//   virtual void flushDebugLines(const Mat4& view, const Mat4& projection) = 0;
// engine/render/backends/vulkan/VulkanRenderer.{h,cpp}:
//   void drawLine(Vec3,Vec3,Vec3) override; -> debugLines_.queue(a,b,color);
//   member: VkDebugLines debugLines_;  init: debugLines_.init(context_, scenePass());
//   destroy: debugLines_.destroy(context_);  endFrame records: debugLines_.record(cb, context_.device(), frames_, pendingDebugView_, pendingDebugProj_);
// engine/render/backends/vulkan/VkDebugLines.{h,cpp}: pipeline_ is depthTestEnable=VK_TRUE, compareOp=LESS,
//   depthWriteEnable=VK_FALSE; struct Vertex{Vec3 position; Vec3 color;}; queued_ vector; queue()/record()/init()/destroy().
//   record() allocates one vertex buffer (frames.allocateVertices), one UBO (frames.allocateUbo), one descriptor set, binds pipeline_, draws, clears.
// engine/editor/Gizmo.{h,cpp} (M31, current): enum class GizmoMode{Translate,Rotate,Scale};
//   bool update(SceneEntity& e, const Ray&, bool pressed, bool down, Vec3 camPos); void draw(Renderer&, const SceneEntity& e, Vec3 camPos);
//   helpers (anon ns): axisDir, axisColor, gizmoSize, segSegDistance, raySegmentDistance, rayAxisParam, rayPlane, ringAngle, drawRing, drawBox.
// engine/math/Vec.h: Vec3 ops + - *scalar, dot, cross, length, normalize. engine/math/Aabb.h: Aabb{Vec3 min,max;}.
// games/11-sandbox/main.cpp (M31): worldAabb(const Aabb&, const Mat4&) file-local helper; resolved[i].localBounds + entityIndex;
//   gizmo.update(scene.entities[selectedIndex], ray, lmbPressed, lmbDown, cam.position) in update lambda; gizmo.draw(renderer, scene.entities[selectedIndex], cam.position) in render lambda; renderer.flushDebugLines(view, proj).
```

---

## File Structure

**Modify:**
- `engine/render/Renderer.h` — add `drawLineOverlay` (default forwards to `drawLine`).
- `engine/render/backends/vulkan/VkDebugLines.h` — overlay pipeline + queue + `queueOverlay`.
- `engine/render/backends/vulkan/VkDebugLines.cpp` — create overlay pipeline; record both queues; destroy.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — override `drawLineOverlay`.
- `engine/editor/Gizmo.h` / `.cpp` — explicit origin, hover, degenerate-hold, overlay draw, highlight.
- `games/11-sandbox/main.cpp` — pass the selected entity's world-AABB center as the gizmo origin.
- `docs/engine/editor.md` — refresh the gizmo notes (Task 4).

No new tests — this is interaction/rendering polish (`iron::Gizmo` is editor-only, not reachable by the `ironcore`-linked harness). The existing 46 tests must stay green.

---

## Task 1: Engine overlay (always-on-top) line path

**Files:**
- Modify: `engine/render/Renderer.h`, `engine/render/backends/vulkan/VkDebugLines.h`, `engine/render/backends/vulkan/VkDebugLines.cpp`, `engine/render/backends/vulkan/VulkanRenderer.h`, `engine/render/backends/vulkan/VulkanRenderer.cpp`

- [ ] **Step 1: Add `drawLineOverlay` to `engine/render/Renderer.h`**

Right after the `flushDebugLines` pure-virtual declaration (the `// --- debug drawing ---` block), add:

```cpp
    // Like drawLine, but drawn on top of geometry (depth test disabled) — for
    // editor gizmos / manipulators that must stay visible. Default forwards to
    // drawLine (depth-tested); the Vulkan backend overrides it with an overlay
    // pass. Flushed by flushDebugLines alongside the regular lines.
    virtual void drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) { drawLine(a, b, color); }
```

- [ ] **Step 2: Add the overlay pipeline + queue to `engine/render/backends/vulkan/VkDebugLines.h`**

Add the `queueOverlay` declaration after `queue`:

```cpp
    void queue(Vec3 a, Vec3 b, Vec3 color);
    // Like queue(), but recorded with depth-test disabled (drawn on top).
    void queueOverlay(Vec3 a, Vec3 b, Vec3 color);
```

In the private members, add the overlay pipeline + queue beside the existing ones:

```cpp
    ::VkPipeline pipeline_ = VK_NULL_HANDLE;
    ::VkPipeline overlayPipeline_ = VK_NULL_HANDLE;  // same as pipeline_ but depthTest off
    std::vector<Vertex> queued_;
    std::vector<Vertex> queuedOverlay_;
```

- [ ] **Step 3: Create the overlay pipeline in `VkDebugLines::init`**

In `VkDebugLines.cpp`, the `init` function builds `pInfo` and creates `pipeline_` (the `vkCreateGraphicsPipelines(... &pipeline_)` call). Immediately AFTER that call (and before `vkDestroyShaderModule`), add a second create with depth test disabled:

```cpp
    // Overlay pipeline: identical state but depth-test disabled, so gizmo lines
    // draw on top of geometry. `ds` and `pInfo` are reused with one field flipped.
    ds.depthTestEnable = VK_FALSE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &overlayPipeline_));
```

(`ds` is the `VkPipelineDepthStencilStateCreateInfo` local already declared above; `pInfo.pDepthStencilState` points at it, so flipping `ds.depthTestEnable` and re-creating yields the overlay variant.)

- [ ] **Step 4: Destroy the overlay pipeline in `VkDebugLines::destroy`**

In `destroy`, add (next to the `pipeline_` destruction):

```cpp
    if (overlayPipeline_) { vkDestroyPipeline(ctx.device(), overlayPipeline_, nullptr); overlayPipeline_ = VK_NULL_HANDLE; }
```

- [ ] **Step 5: Add `queueOverlay` and rework `record` to draw both queues**

Add `queueOverlay` next to `queue`:

```cpp
void VkDebugLines::queueOverlay(Vec3 a, Vec3 b, Vec3 color) {
    queuedOverlay_.push_back({a, color});
    queuedOverlay_.push_back({b, color});
}
```

Replace the entire body of `VkDebugLines::record(...)` with this version (shared UBO + descriptor set; draws the depth-tested queue then the overlay queue; clears both):

```cpp
void VkDebugLines::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                          const Mat4& view, const Mat4& projection) {
    if (!ok_ || cb == VK_NULL_HANDLE || (queued_.empty() && queuedOverlay_.empty())) {
        queued_.clear();
        queuedOverlay_.clear();
        return;
    }

    // One UBO + descriptor set shared by both passes (same view-projection).
    CameraUbo ubo;
    const Mat4 vp = projection * view;
    std::memcpy(ubo.viewProjection, vp.m, sizeof(float) * 16);
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

    VkDescriptorSetAllocateInfo daInfo{};
    daInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    daInfo.descriptorPool = frames.current().descriptorPool;
    daInfo.descriptorSetCount = 1;
    daInfo.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &daInfo, &set));

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = frames.current().uboBuffer;
    uboInfo.offset = uboOffset;
    uboInfo.range = sizeof(ubo);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &uboInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &set, 0, nullptr);

    // Draw one queue with the given pipeline. Allocates its own vertex range.
    auto drawQueue = [&](std::vector<Vertex>& q, ::VkPipeline pipe) {
        if (q.empty()) return;
        VkDeviceSize voff = 0;
        VkBuffer vb = frames.allocateVertices(q.data(), q.size() * sizeof(Vertex), voff);
        if (vb == VK_NULL_HANDLE) {
            Log::warn("VkDebugLines: vertex sub-allocator overflow, skipping a queue");
            return;
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
        vkCmdDraw(cb, static_cast<std::uint32_t>(q.size()), 1, 0, 0);
    };

    drawQueue(queued_, pipeline_);              // depth-tested
    drawQueue(queuedOverlay_, overlayPipeline_); // always-on-top

    queued_.clear();
    queuedOverlay_.clear();
}
```

- [ ] **Step 6: Override `drawLineOverlay` in `VulkanRenderer`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, after the `void drawLine(...) override;` line, add:

```cpp
    void drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) override;
```

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, after the `VulkanRenderer::drawLine` definition, add:

```cpp
void VulkanRenderer::drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) {
    debugLines_.queueOverlay(a, b, color);
}
```

(The existing `flushDebugLines` → `endFrame` → `debugLines_.record(...)` path now records both queues — no change needed there.)

- [ ] **Step 7: Build the engine**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target ironcore --config Debug
```

Expected: clean compile of `VkDebugLines.cpp` + `VulkanRenderer.cpp` with the overlay pipeline.

- [ ] **Step 8: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 46/46 green (no behavior change to existing tests).

- [ ] **Step 9: Commit**

```powershell
git add engine/render/Renderer.h engine/render/backends/vulkan/VkDebugLines.h engine/render/backends/vulkan/VkDebugLines.cpp engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M32 Task 1: depth-disabled overlay line path (drawLineOverlay) for always-on-top gizmos"
```

---

## Task 2: Gizmo — origin, hover, smooth drag, overlay draw, highlight

**Files:**
- Modify: `engine/editor/Gizmo.h`, `engine/editor/Gizmo.cpp`

Replace both files wholesale (the changes touch most functions; full files avoid ambiguity).

- [ ] **Step 1: Replace `engine/editor/Gizmo.h`**

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vec.h"

namespace iron {

struct SceneEntity;
class Renderer;

enum class GizmoMode { Translate, Rotate, Scale };

// A world-axis transform gizmo for the editor. Operates on a single SceneEntity:
// hit-tests its handles against a mouse ray (for hover + click-grab), drags
// translate/rotate/scale, and draws itself as always-on-top debug lines at an
// explicit origin (the entity's visible-bounds center, supplied by the host).
class Gizmo {
public:
    void setMode(GizmoMode m);          // ignored mid-drag
    GizmoMode mode() const { return mode_; }
    bool dragging() const { return axis_ >= 0; }

    // Per-frame input. `origin` is where the gizmo is drawn + hit-tested (the
    // selected entity's world-AABB center). `mousePressed` = LMB went down this
    // frame; `mouseDown` = LMB held. Updates the hovered handle every frame; on a
    // press over a handle it begins a drag and returns true (host should NOT
    // re-select); while dragging it applies the transform to `e` and returns
    // true. Returns false when not consuming the mouse.
    bool update(SceneEntity& e, Vec3 origin, const Ray& mouseRay,
                bool mousePressed, bool mouseDown, Vec3 camPos);

    // Emit the gizmo at `origin` as always-on-top debug lines; the hovered/active
    // handle is brightened.
    void draw(Renderer& renderer, Vec3 origin, Vec3 camPos) const;

private:
    GizmoMode mode_ = GizmoMode::Translate;
    int   axis_ = -1;            // dragging axis 0/1/2; -1 = idle
    int   hoveredAxis_ = -1;     // handle under the cursor (for highlight), -1 = none
    float startParam_ = 0.0f;    // axis param (translate/scale) or angle (rotate) at drag start
    float lastParam_ = 0.0f;     // last valid param this drag (held on degenerate solves)
    Vec3  startPos_{};
    Vec3  startScale_{};
    Vec3  startOrigin_{};        // gizmo origin captured at drag start (stable axis line)
    Quat  startRot_{};
};

}  // namespace iron
```

- [ ] **Step 2: Replace `engine/editor/Gizmo.cpp`**

```cpp
#include "editor/Gizmo.h"

#include "render/Renderer.h"
#include "scene/SceneFormat.h"

#include <cmath>

namespace iron {

namespace {

constexpr float kGizmoScreenScale = 0.15f;  // gizmo world size ~= this * camera distance
constexpr float kHandlePickFrac   = 0.25f;  // forgiving pick threshold (fraction of gizmo size)
constexpr float kTwoPi            = 6.28318530718f;

Vec3 axisDir(int a)   { return a == 0 ? Vec3{1, 0, 0} : a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}; }
Vec3 axisColor(int a) { return a == 0 ? Vec3{1.0f, 0.25f, 0.25f}
                             : a == 1 ? Vec3{0.25f, 1.0f, 0.25f}
                                      : Vec3{0.35f, 0.45f, 1.0f}; }

float gizmoSize(Vec3 camPos, Vec3 origin) {
    const float d = length(camPos - origin) * kGizmoScreenScale;
    return d < 0.001f ? 0.001f : d;
}

// Closest distance between two segments (Ericson, Real-Time Collision Detection 5.1.9).
float segSegDistance(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2) {
    const Vec3 d1 = q1 - p1;
    const Vec3 d2 = q2 - p2;
    const Vec3 r  = p1 - p2;
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    const float EPS = 1e-8f;
    float s = 0.0f, t = 0.0f;
    if (a <= EPS && e <= EPS) {
        // both points
    } else if (a <= EPS) {
        t = f / e; if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    } else {
        const float c = dot(d1, r);
        if (e <= EPS) {
            s = -c / a; if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom > EPS) {
                s = (b * f - c * e) / denom;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f; s = -c / a;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            } else if (t > 1.0f) {
                t = 1.0f; s = (b - c) / a;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            }
        }
    }
    const Vec3 c1 = p1 + d1 * s;
    const Vec3 c2 = p2 + d2 * t;
    return length(c1 - c2);
}

float raySegmentDistance(const Ray& ray, Vec3 a, Vec3 b) {
    return segSegDistance(ray.origin, ray.origin + ray.direction * 1.0e4f, a, b);
}

// Param along the infinite line (origin + dir*p, dir unit) of the point closest
// to the ray. Returns false (and leaves `out` untouched) when the ray is
// near-parallel to the axis (ill-conditioned).
bool rayAxisParam(const Ray& ray, Vec3 origin, Vec3 dir, float& out) {
    const Vec3 r = ray.origin - origin;
    const float b = dot(ray.direction, dir);
    const float c = dot(ray.direction, r);
    const float f = dot(dir, r);
    const float denom = 1.0f - b * b;
    if (denom < 1e-4f) return false;
    out = (f - b * c) / denom;
    return true;
}

bool rayPlane(const Ray& ray, Vec3 origin, Vec3 n, Vec3& hit) {
    const float denom = dot(ray.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(origin - ray.origin, n) / denom;
    if (t < 0.0f) return false;
    hit = ray.origin + ray.direction * t;
    return true;
}

// Angle of the ray's hit point on the rotation plane of `axis` about `origin`.
// Returns false when the ray is parallel to the plane.
bool ringAngle(const Ray& ray, Vec3 origin, int axis, float& out) {
    Vec3 hit;
    if (!rayPlane(ray, origin, axisDir(axis), hit)) return false;
    Vec3 u, v;
    if (axis == 0)      { u = Vec3{0, 1, 0}; v = Vec3{0, 0, 1}; }
    else if (axis == 1) { u = Vec3{0, 0, 1}; v = Vec3{1, 0, 0}; }
    else                { u = Vec3{1, 0, 0}; v = Vec3{0, 1, 0}; }
    const Vec3 d = hit - origin;
    out = std::atan2(dot(d, v), dot(d, u));
    return true;
}

// Which handle (axis 0/1/2) is under the ray for the given mode, or -1.
int pickHandle(GizmoMode mode, const Ray& ray, Vec3 origin, float size) {
    int picked = -1;
    if (mode == GizmoMode::Translate || mode == GizmoMode::Scale) {
        float best = size * kHandlePickFrac;
        for (int a = 0; a < 3; ++a) {
            const float d = raySegmentDistance(ray, origin, origin + axisDir(a) * size);
            if (d < best) { best = d; picked = a; }
        }
    } else {  // Rotate
        float best = size * kHandlePickFrac;
        for (int a = 0; a < 3; ++a) {
            Vec3 hit;
            if (rayPlane(ray, origin, axisDir(a), hit)) {
                const float err = std::fabs(length(hit - origin) - size);
                if (err < best) { best = err; picked = a; }
            }
        }
    }
    return picked;
}

void drawRing(Renderer& r, Vec3 c, Vec3 axis, float radius, Vec3 color) {
    Vec3 seed = std::fabs(axis.x) > 0.5f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 u = normalize(seed - axis * dot(seed, axis));
    Vec3 v = cross(axis, u);
    const int N = 32;
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / N * kTwoPi;
        const Vec3 p = c + (u * std::cos(t) + v * std::sin(t)) * radius;
        r.drawLineOverlay(prev, p, color);
        prev = p;
    }
}

void drawBox(Renderer& r, Vec3 c, float h, Vec3 color) {
    const Vec3 k[8] = {
        {c.x - h, c.y - h, c.z - h}, {c.x + h, c.y - h, c.z - h},
        {c.x + h, c.y + h, c.z - h}, {c.x - h, c.y + h, c.z - h},
        {c.x - h, c.y - h, c.z + h}, {c.x + h, c.y - h, c.z + h},
        {c.x + h, c.y + h, c.z + h}, {c.x - h, c.y + h, c.z + h},
    };
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& pr : e) r.drawLineOverlay(k[pr[0]], k[pr[1]], color);
}

}  // namespace

void Gizmo::setMode(GizmoMode m) { if (axis_ < 0) mode_ = m; }

bool Gizmo::update(SceneEntity& e, Vec3 origin, const Ray& ray,
                   bool mousePressed, bool mouseDown, Vec3 camPos) {
    const float size = gizmoSize(camPos, origin);

    if (!mouseDown) {
        axis_ = -1;
        hoveredAxis_ = pickHandle(mode_, ray, origin, size);  // hover feedback
        return false;
    }

    // Begin a drag on press over a handle.
    if (mousePressed && axis_ < 0) {
        const int picked = pickHandle(mode_, ray, origin, size);
        hoveredAxis_ = picked;
        if (picked < 0) return false;   // empty press -> host re-selects
        axis_        = picked;
        startPos_    = e.position;
        startScale_  = e.scale;
        startRot_    = e.rotation;
        startOrigin_ = origin;
        float p = 0.0f;
        if (mode_ == GizmoMode::Rotate) ringAngle(ray, startOrigin_, picked, p);
        else                            rayAxisParam(ray, startOrigin_, axisDir(picked), p);
        startParam_ = p;
        lastParam_  = p;
        return true;
    }

    // Continue a drag. On a degenerate/near-parallel solve, hold the last param
    // (no movement this frame) instead of jumping.
    if (axis_ >= 0) {
        const Vec3 dir = axisDir(axis_);
        if (mode_ == GizmoMode::Translate) {
            float p;
            if (rayAxisParam(ray, startOrigin_, dir, p)) lastParam_ = p; else p = lastParam_;
            e.position = startPos_ + dir * (p - startParam_);
        } else if (mode_ == GizmoMode::Scale) {
            float p;
            if (rayAxisParam(ray, startOrigin_, dir, p)) lastParam_ = p; else p = lastParam_;
            float s = (axis_ == 0 ? startScale_.x : axis_ == 1 ? startScale_.y : startScale_.z) + (p - startParam_);
            if (s < 0.01f) s = 0.01f;
            if (axis_ == 0) e.scale.x = s; else if (axis_ == 1) e.scale.y = s; else e.scale.z = s;
        } else {  // Rotate
            float ang;
            if (ringAngle(ray, startOrigin_, axis_, ang)) lastParam_ = ang; else ang = lastParam_;
            e.rotation = Quat::fromAxisAngle(dir, ang - startParam_) * startRot_;
        }
        return true;
    }
    return false;
}

void Gizmo::draw(Renderer& renderer, Vec3 origin, Vec3 camPos) const {
    const float size = gizmoSize(camPos, origin);
    const int highlight = (axis_ >= 0) ? axis_ : hoveredAxis_;

    auto colorFor = [&](int a) -> Vec3 {
        const Vec3 c = axisColor(a);
        if (a == highlight) {  // brighten the hovered/active handle
            return Vec3{c.x + 0.4f > 1.0f ? 1.0f : c.x + 0.4f,
                        c.y + 0.4f > 1.0f ? 1.0f : c.y + 0.4f,
                        c.z + 0.4f > 1.0f ? 1.0f : c.z + 0.4f};
        }
        return c * 0.55f;  // dim the others
    };

    if (mode_ == GizmoMode::Rotate) {
        for (int a = 0; a < 3; ++a) drawRing(renderer, origin, axisDir(a), size, colorFor(a));
        return;
    }
    for (int a = 0; a < 3; ++a) {
        const Vec3 tip = origin + axisDir(a) * size;
        renderer.drawLineOverlay(origin, tip, colorFor(a));
        if (mode_ == GizmoMode::Scale) drawBox(renderer, tip, size * 0.08f, colorFor(a));
    }
}

}  // namespace iron
```

- [ ] **Step 3: Build the editor lib**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target ironcore_editor --config Debug
```

Expected: clean compile. (`drawLineOverlay` exists on `Renderer` from Task 1.) This WILL break the `sandbox` target until Task 3 updates the call sites (`gizmo.update`/`gizmo.draw` signatures changed) — that's expected; only build `ironcore_editor` here.

- [ ] **Step 4: Commit**

```powershell
git add engine/editor/Gizmo.h engine/editor/Gizmo.cpp
git commit -m "M32 Task 2: gizmo hover + explicit origin + smooth drag (degenerate hold) + overlay draw"
```

---

## Task 3: Sandbox — gizmo origin at world-AABB center + per-frame hover

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Add a `gizmoOriginFor` helper before the update lambda**

The sandbox already has the file-local `worldAabb(const iron::Aabb&, const iron::Mat4&)` helper and the `resolved` vector (each with `entityIndex` + `localBounds`). Add a lambda that returns the selected entity's world-AABB center (or its pivot if it has no resolved mesh). Place it just BEFORE `app.setUpdate(...)`, alongside the other locals (it captures `scene` + `resolved` by reference):

```cpp
    // World-AABB center of an entity (where the gizmo is drawn / hit-tested), or
    // its pivot if the entity has no resolved mesh.
    auto gizmoOriginFor = [&](int sel) -> iron::Vec3 {
        const iron::SceneEntity& e = scene.entities[sel];
        const iron::Mat4 model = iron::translation(e.position)
                               * e.rotation.toMat4()
                               * iron::scaling(e.scale);
        for (const auto& re : resolved) {
            if (re.entityIndex == sel) {
                const iron::Aabb wa = worldAabb(re.localBounds, model);
                return iron::Vec3{(wa.min.x + wa.max.x) * 0.5f,
                                  (wa.min.y + wa.max.y) * 0.5f,
                                  (wa.min.z + wa.max.z) * 0.5f};
            }
        }
        return e.position;
    };
```

- [ ] **Step 2: Pass the origin to `gizmo.update` in the update lambda**

In the editor-interaction block (inside `if (!look) { ... if (!uiBusy || gizmo.dragging()) { ... } }`), the M31 call is:

```cpp
                bool consumed = false;
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    consumed = gizmo.update(scene.entities[selectedIndex], ray,
                                            lmbPressed, lmbDown, cam.position);
```

Replace it with (insert the `gizmoOriginFor(selectedIndex)` origin argument):

```cpp
                bool consumed = false;
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    consumed = gizmo.update(scene.entities[selectedIndex],
                                            gizmoOriginFor(selectedIndex), ray,
                                            lmbPressed, lmbDown, cam.position);
```

(Hover now updates every frame this block runs, since `gizmo.update` is called whenever a selection exists and the mouse isn't busy / is dragging.)

- [ ] **Step 3: Pass the origin to `gizmo.draw` in the render lambda**

The M31 render call is:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, scene.entities[selectedIndex], cam.position);
        renderer.flushDebugLines(view, proj);
```

Replace the `gizmo.draw` line with:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex), cam.position);
```

(Leave the `renderer.flushDebugLines(view, proj);` line as-is.)

- [ ] **Step 4: Build**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target sandbox --config Debug
```

Expected: clean build of `sandbox` (the new `gizmo.update`/`gizmo.draw` signatures now match).

- [ ] **Step 5: Visual check (human verifies; subagent confirms build only)**

```powershell
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

Expected (user-verified): the gizmo is visible through geometry; it sits on the selected mesh and stays put as the camera pitches; hovering a handle brightens it; clicking a (highlighted) handle grabs it and never deselects; dragging is smooth with no jumps at grazing angles. The subagent only confirms the build succeeds (do NOT run the exe).

- [ ] **Step 6: Commit**

```powershell
git add games/11-sandbox/main.cpp
git commit -m "M32 Task 3: gizmo at world-AABB center + per-frame hover in the sandbox editor"
```

---

## Task 4: Docs + PR

**Files:**
- Modify: `docs/engine/editor.md`

- [ ] **Step 1: Refresh the gizmo notes in `docs/engine/editor.md`**

Read the existing "Viewport gizmos + selection" section (added in M31) and update it for the M32 polish:
- The gizmo now renders **always-on-top** (depth-disabled overlay line path; `Renderer::drawLineOverlay`).
- Handles **highlight on hover**; clicking a highlighted handle grabs it (so a click on a handle never deselects).
- The gizmo is drawn at the selected entity's **world-AABB center** (sits on the visible mesh), and dragging is smooth (the gizmo holds the last axis param on degenerate/grazing solves).
- Remove the now-resolved limitation bullet ("gizmo depth follows the debug-line pass / may be occluded"); keep the remaining ones (loose world-AABB picking; world-axis only; rotate/scale about the pivot though drawn at bounds center; no planar handles / snapping / multi-select / undo).

Match the existing doc's prose style.

- [ ] **Step 2: Commit, push, open PR**

```powershell
git add docs/engine/editor.md
git commit -m "M32: document gizmo polish (always-on-top, hover, centered, smooth drag)"
git push -u origin feat/m32-gizmo-polish
```

Open the PR (match the M31 #52 template). Title: `M32: Gizmo polish (always-on-top, hover, smooth drag)`. Body:

```
## Summary

- Gizmo now renders **always-on-top** of geometry via a new depth-disabled overlay line path (`Renderer::drawLineOverlay` + a second `VkDebugLines` pipeline).
- Handles **highlight on hover**; clicking a highlighted handle grabs it — fixes the M31 bug where clicking a handle deselected the object.
- Gizmo is drawn at the selected entity's **world-AABB center** so it sits on the visible mesh and stays put as the camera pitches.
- Dragging is smooth — the gizmo holds the last axis param on degenerate/grazing solves instead of jumping (fixes the M31 twitch).

## Test plan

- [x] Full suite green (46/46) — engine overlay path is non-breaking
- [x] ironcore + ironcore_editor + sandbox build clean
- [ ] Visual: gizmo visible through geometry; hover highlights; click-grab never deselects; drag smooth; gizmo centered on the mesh

## Known v1 limitations

- Loose world-AABB picking; world-axis gizmos only; no planar handles, snapping, multi-select, or undo.
- Rotate/scale operate about the entity pivot though the gizmo is drawn at the bounds center (visually fine when pivot ≈ center).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — 46/46 green.
- [ ] `ironcore`, `ironcore_editor`, `sandbox` build clean.
- [ ] Visual: always-on-top gizmo; hover highlight; click-grab (no deselect); smooth drag; centered on the mesh; hold-RMB still flies.
- [ ] PR CI green.
