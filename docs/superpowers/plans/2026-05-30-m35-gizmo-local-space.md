# M35 — Oriented Selection Outline + Gizmo Local/World Space Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the editor's selection outline rotate/scale with the object, and add an Unreal-style World/Local space toggle for the translate/rotate gizmo handles (key `X` + an Inspector toggle).

**Architecture:** Three surfaces change. (1) The sandbox selection outline switches from a re-derived world-AABB to a per-corner model-transformed oriented box — sandbox-only. (2) `iron::Gizmo` gains a `GizmoSpace { World, Local }` and computes a 3-axis **basis** (`Vec3 ax[3]`) — world axes, or the entity's rotated local axes — that is threaded through every pick/draw/apply path in place of the hard-wired `axisDir(a)`. Scale is always local. During a drag the basis is frozen to `startRot_` so a rotate drag doesn't chase its own rotation; the conjugation identity `fromAxisAngle(R·a,θ)·R == R·fromAxisAngle(a,θ)` makes World mode reduce exactly to today's behavior. (3) `SceneInspector::draw` takes `GizmoSpace&` and shows a World/Local toggle; the host treats the gizmo as the single source of truth.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), Vulkan 1.3, Dear ImGui (editor module only). Reference spec: `docs/superpowers/specs/2026-05-30-m35-gizmo-local-space-design.md`.

**Verification model:** The gizmo and the outline are interactive and sandbox-side — not reachable by the `ironcore`-linked test harness (consistent with M31–M34). So each task's gate is: the project **builds clean** (`cmake --build build-vk`) and the **existing 46 tests stay green** (`ctest --test-dir build-vk`). Final acceptance is **user visual verification** (Task 5).

**Build & test commands (used by every task):**

```bash
cmake --build build-vk -j
ctest --test-dir build-vk --output-on-failure
```

(If `build-vk` is stale/missing, configure first: `cmake -S . -B build-vk`.)

---

## File Structure

**Modify only — no new files:**

- `games/11-sandbox/main.cpp` — oriented outline (Task 1); `X` key toggle + pass rotation to `gizmo.draw` (Task 2); Inspector space wiring (Task 3).
- `engine/editor/Gizmo.h` — `GizmoSpace` enum, `setSpace`/`space`/`toggleSpace`, `space_` member, `draw()` gains a `Quat rotation` arg (Task 2).
- `engine/editor/Gizmo.cpp` — `buildBasis` helper; thread `const Vec3 ax[3]` through `pickHandle`/`planeHandleHit`/`planeAxes`/`ringAngle`; oriented apply in `update()`; oriented `draw()`; `setSpace`/`toggleSpace` (Task 2).
- `engine/editor/SceneInspector.h` / `.cpp` — `draw(SceneEntity&, GizmoSpace&)` + World/Local header toggle (Task 3).
- `docs/engine/editor.md` — document the toggle + oriented outline (Task 4).

Each task leaves the tree **buildable** at its commit (signature changes and their call sites move together within one task).

---

## Task 1: Oriented selection outline (sandbox)

Today the outline (`games/11-sandbox/main.cpp:631–651`) computes a world-axis AABB via `worldAabb(re.localBounds, m)`, then draws that AABB's 8 corners — so rotating the entity just grows/shrinks the box. Fix: transform **each local-bounds corner** by the model matrix `m` and draw the same 12 edges between the transformed corners. The corner bit-ordering is identical to today's, so the existing `edges[12][2]` table is reused unchanged.

**Files:**
- Modify: `games/11-sandbox/main.cpp:631-651`

- [ ] **Step 1: Replace the outline body with a per-corner oriented box**

Find this block (currently at `main.cpp:631-651`):

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            for (const auto& re : resolved) {
                if (re.entityIndex != selectedIndex) continue;
                const iron::SceneEntity& se = scene.entities[selectedIndex];
                const iron::Mat4 m = iron::translation(se.position)
                                   * se.rotation.toMat4()
                                   * iron::scaling(se.scale);
                const iron::Aabb wa = worldAabb(re.localBounds, m);
                iron::Vec3 c[8];
                for (int i = 0; i < 8; ++i)
                    c[i] = iron::Vec3{(i & 1) ? wa.max.x : wa.min.x,
                                      (i & 2) ? wa.max.y : wa.min.y,
                                      (i & 4) ? wa.max.z : wa.min.z};
                const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                          {4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                const iron::Vec3 outline{1.0f, 0.6f, 0.1f};  // selection orange
                for (auto& ed : edges)
                    renderer.drawLineOverlay(c[ed[0]], c[ed[1]], outline);
                break;
            }
        }
```

Replace it with (only the corner computation changes — `worldAabb` is gone, each local corner is transformed by `m`):

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            for (const auto& re : resolved) {
                if (re.entityIndex != selectedIndex) continue;
                const iron::SceneEntity& se = scene.entities[selectedIndex];
                const iron::Mat4 m = iron::translation(se.position)
                                   * se.rotation.toMat4()
                                   * iron::scaling(se.scale);
                // Oriented bounding box: transform each LOCAL-bounds corner by the
                // model matrix so the outline rotates + scales with the object,
                // instead of a world-axis AABB that just grows/shrinks on spin.
                const iron::Aabb& lb = re.localBounds;
                iron::Vec3 c[8];
                for (int i = 0; i < 8; ++i) {
                    const iron::Vec3 lc{(i & 1) ? lb.max.x : lb.min.x,
                                        (i & 2) ? lb.max.y : lb.min.y,
                                        (i & 4) ? lb.max.z : lb.min.z};
                    const iron::Vec4 w = m * iron::Vec4{lc.x, lc.y, lc.z, 1.0f};
                    c[i] = iron::Vec3{w.x, w.y, w.z};  // model has no perspective; w == 1
                }
                const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                          {4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                const iron::Vec3 outline{1.0f, 0.6f, 0.1f};  // selection orange
                for (auto& ed : edges)
                    renderer.drawLineOverlay(c[ed[0]], c[ed[1]], outline);
                break;
            }
        }
```

> Note: `worldAabb` is still used for click-picking (`main.cpp:517-523`), so leave that helper in place — do not delete it.

- [ ] **Step 2: Build**

Run: `cmake --build build-vk -j`
Expected: builds clean, 0 errors.

- [ ] **Step 3: Verify the existing tests still pass**

Run: `ctest --test-dir build-vk --output-on-failure`
Expected: all 46 pass (this task doesn't touch tested code; this is a regression guard).

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M35: oriented selection outline (transform local-bounds corners)"
```

---

## Task 2: Gizmo local/world space (engine + gizmo host wiring)

The core change. `Gizmo` computes `Vec3 ax[3]` (the handle basis) and uses it everywhere it currently calls `axisDir(a)` for handle geometry. World mode → `ax[a] = axisDir(a)` (today's behavior). Local mode → `ax[a] = rot.rotate(axisDir(a))`. Scale is always local. The basis is built from the live entity rotation while idle/picking and frozen to `startRot_` during a drag. `draw()` gains a `Quat rotation` argument so the host can hand it the entity's rotation. The sandbox call sites (`gizmo.draw` + the new `X` key) move in this same task so the tree builds.

**Files:**
- Modify: `engine/editor/Gizmo.h`
- Modify: `engine/editor/Gizmo.cpp`
- Modify: `games/11-sandbox/main.cpp:491-493` (X key) and `main.cpp:626-627` (draw call)

- [ ] **Step 1: Add `GizmoSpace`, the new methods, the member, and the `draw` signature (`Gizmo.h`)**

In `engine/editor/Gizmo.h`, change the mode enum line:

```cpp
enum class GizmoMode { Translate, Rotate, Scale };
```

to add the space enum right after it:

```cpp
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };
```

Then in `class Gizmo`, find:

```cpp
    void setMode(GizmoMode m);          // ignored mid-drag
    GizmoMode mode() const { return mode_; }
    bool dragging() const { return axis_ >= 0; }
```

and add the space accessors below them:

```cpp
    void setMode(GizmoMode m);          // ignored mid-drag
    GizmoMode mode() const { return mode_; }
    bool dragging() const { return axis_ >= 0; }

    void       setSpace(GizmoSpace s);  // ignored mid-drag, like setMode
    GizmoSpace space() const { return space_; }
    void       toggleSpace();           // World<->Local, ignored mid-drag
```

Change the `draw` declaration:

```cpp
    void draw(Renderer& renderer, Vec3 origin, Vec3 camPos) const;
```

to take the entity rotation (so it can orient the handles):

```cpp
    // `rotation` orients the handles in Local space (and is ignored in World
    // space / for Scale, which is always local).
    void draw(Renderer& renderer, Vec3 origin, Quat rotation, Vec3 camPos) const;
```

Add the private member next to `mode_`:

```cpp
    GizmoMode mode_ = GizmoMode::Translate;
```

becomes:

```cpp
    GizmoMode  mode_  = GizmoMode::Translate;
    GizmoSpace space_ = GizmoSpace::World;
```

- [ ] **Step 2: Add the `buildBasis` helper (`Gizmo.cpp`)**

In the anonymous namespace in `engine/editor/Gizmo.cpp`, right after `axisColor` (currently `Gizmo.cpp:25-27`), add:

```cpp
// Fill ax[0..2] with the gizmo's handle axes. World: the canonical X/Y/Z. Local:
// the entity's local axes (its rotation applied), expressed in world space.
void buildBasis(bool local, const Quat& rot, Vec3 ax[3]) {
    for (int a = 0; a < 3; ++a)
        ax[a] = local ? rot.rotate(axisDir(a)) : axisDir(a);
}
```

- [ ] **Step 3: Thread the basis through `ringAngle` and `planeAxes`**

Replace `ringAngle` (currently `Gizmo.cpp:108-120`) — it must use the oriented normal `ax[axis]` and the oriented in-plane axes instead of hard-coded world vectors:

```cpp
// Angle of the ray's hit point on the rotation plane of `axis` about `origin`,
// measured in the oriented basis `ax`. Returns false when the ray is parallel
// to the plane.
bool ringAngle(const Ray& ray, Vec3 origin, const Vec3 ax[3], int axis, float& out) {
    Vec3 hit;
    if (!rayPlane(ray, origin, ax[axis], hit)) return false;
    Vec3 u, v;
    if (axis == 0)      { u = ax[1]; v = ax[2]; }   // about X: (Y,Z)
    else if (axis == 1) { u = ax[2]; v = ax[0]; }   // about Y: (Z,X)
    else                { u = ax[0]; v = ax[1]; }   // about Z: (X,Y)
    const Vec3 d = hit - origin;
    out = std::atan2(dot(d, v), dot(d, u));
    return true;
}
```

Replace `planeAxes` (currently `Gizmo.cpp:122-127`) to index the basis:

```cpp
// The two in-plane axes of the plane whose normal is basis-axis `n`.
void planeAxes(const Vec3 ax[3], int n, Vec3& u, Vec3& v) {
    if (n == 0)      { u = ax[1]; v = ax[2]; }  // YZ
    else if (n == 1) { u = ax[0]; v = ax[2]; }  // XZ (horizontal)
    else             { u = ax[0]; v = ax[1]; }  // XY
}
```

- [ ] **Step 4: Thread the basis through `planeHandleHit` and `pickHandle`**

Replace `planeHandleHit` (currently `Gizmo.cpp:131-139`):

```cpp
// True if the planar handle for basis-axis `n` is under the ray. The square sits
// in the [inset, reach] corner of the two in-plane axes near origin.
bool planeHandleHit(const Ray& ray, Vec3 origin, const Vec3 ax[3], int n, float size) {
    Vec3 hit;
    if (!rayPlane(ray, origin, ax[n], hit)) return false;
    Vec3 u, v; planeAxes(ax, n, u, v);
    const Vec3 d = hit - origin;
    const float du = dot(d, u), dv = dot(d, v);
    const float lo = kPlaneInset * size, hi = kPlaneReach * size;
    return du >= lo && du <= hi && dv >= lo && dv <= hi;
}
```

Replace `pickHandle` (currently `Gizmo.cpp:142-170`):

```cpp
// Which handle is under the ray for the given mode, or -1.
int pickHandle(GizmoMode mode, const Ray& ray, Vec3 origin, const Vec3 ax[3], float size) {
    if (mode == GizmoMode::Translate) {
        // Center free-move handle (innermost) wins right at the origin.
        if (rayPointDistance(ray, origin) < size * kCenterPickFrac) return kCenterHandle;
        // Then the planar corner squares.
        for (int n = 0; n < 3; ++n)
            if (planeHandleHit(ray, origin, ax, n, size)) return 3 + n;
    }
    if (mode == GizmoMode::Translate || mode == GizmoMode::Scale) {
        int picked = -1;
        float best = size * kHandlePickFrac;
        for (int a = 0; a < 3; ++a) {
            const float d = raySegmentDistance(ray, origin, origin + ax[a] * size);
            if (d < best) { best = d; picked = a; }
        }
        return picked;
    }
    // Rotate: rings.
    int picked = -1;
    float best = size * kHandlePickFrac;
    for (int a = 0; a < 3; ++a) {
        Vec3 hit;
        if (rayPlane(ray, origin, ax[a], hit)) {
            const float err = std::fabs(length(hit - origin) - size);
            if (err < best) { best = err; picked = a; }
        }
    }
    return picked;
}
```

- [ ] **Step 5: Add `setSpace` / `toggleSpace`**

After `void Gizmo::setMode(GizmoMode m) { if (axis_ < 0) mode_ = m; }` (currently `Gizmo.cpp:222`), add:

```cpp
void Gizmo::setSpace(GizmoSpace s) { if (axis_ < 0) space_ = s; }
void Gizmo::toggleSpace() {
    if (axis_ < 0)
        space_ = (space_ == GizmoSpace::World) ? GizmoSpace::Local : GizmoSpace::World;
}
```

- [ ] **Step 6: Rewrite `update()` to use the basis**

Replace the whole body of `Gizmo::update` (currently `Gizmo.cpp:224-295`) with this. Changes vs. today: an `effectiveLocal` flag; `buildBasis` from `e.rotation` at the top (idle/pick) and from `startRot_` on drag-continue; every `axisDir(...)` swapped for `ax[...]`; `pickHandle`/`ringAngle` now take `ax`.

```cpp
bool Gizmo::update(SceneEntity& e, Vec3 origin, const Ray& ray,
                   bool mousePressed, bool mouseDown, Vec3 camPos) {
    const float size = gizmoSize(camPos, origin);
    // Scale is inherently per-local-axis (matches Unreal/Unity, whose world/local
    // toggle is disabled for scale).
    const bool effectiveLocal = (mode_ == GizmoMode::Scale) || (space_ == GizmoSpace::Local);

    // Idle / pre-drag: orient handles by the entity's CURRENT rotation.
    Vec3 ax[3];
    buildBasis(effectiveLocal, e.rotation, ax);

    if (!mouseDown) {
        axis_ = -1;
        hoveredAxis_ = pickHandle(mode_, ray, origin, ax, size);  // hover feedback
        return false;
    }

    // Begin a drag on press over a handle.
    if (mousePressed && axis_ < 0) {
        const int picked = pickHandle(mode_, ray, origin, ax, size);
        hoveredAxis_ = picked;
        if (picked < 0) return false;   // empty press -> host re-selects
        axis_        = picked;
        startPos_    = e.position;
        startScale_  = e.scale;
        startRot_    = e.rotation;
        startOrigin_ = origin;
        if (picked == kCenterHandle) {        // center free-move: camera-facing plane
            startNormal_ = normalize(camPos - startOrigin_);
            Vec3 hit;
            startHit_ = rayPlane(ray, startOrigin_, startNormal_, hit) ? hit : startOrigin_;
        } else if (picked >= 3) {             // planar: the handle's oriented plane
            Vec3 hit;
            startHit_ = rayPlane(ray, startOrigin_, ax[picked - 3], hit) ? hit : startOrigin_;
        } else {
            float p = 0.0f;
            if (mode_ == GizmoMode::Rotate) ringAngle(ray, startOrigin_, ax, picked, p);
            else                            rayAxisParam(ray, startOrigin_, ax[picked], p);
            startParam_ = p;
            lastParam_  = p;
        }
        return true;
    }

    // Continue a drag. Freeze the basis to startRot_ so the handle frame is fixed
    // for the whole drag (a Rotate drag changes e.rotation every frame). On a
    // degenerate/near-parallel solve, hold the last value (no movement this frame).
    if (axis_ >= 0) {
        buildBasis(effectiveLocal, startRot_, ax);
        if (axis_ == kCenterHandle) {  // free-move in the camera-facing plane
            Vec3 hit;
            if (rayPlane(ray, startOrigin_, startNormal_, hit))
                e.position = startPos_ + (hit - startHit_);
            return true;
        }
        if (axis_ >= 3) {  // planar drag: move in the handle's oriented plane
            Vec3 hit;
            if (rayPlane(ray, startOrigin_, ax[axis_ - 3], hit))
                e.position = startPos_ + (hit - startHit_);
            return true;
        }
        const Vec3 dir = ax[axis_];
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
            if (ringAngle(ray, startOrigin_, ax, axis_, ang)) lastParam_ = ang; else ang = lastParam_;
            // ax derived from startRot_, so this is exactly a local-frame rotation
            // about the object's own axis; in World mode ax is the world axis and
            // it reduces to today's behavior.
            e.rotation = Quat::fromAxisAngle(dir, ang - startParam_) * startRot_;
        }
        return true;
    }
    return false;
}
```

- [ ] **Step 7: Rewrite `draw()` to use the basis**

Replace the `Gizmo::draw` signature + body (currently `Gizmo.cpp:297-363`). Changes vs. today: new `Quat rotation` parameter; `effectiveLocal`; `buildBasis` from `startRot_` while dragging else from `rotation`; every `axisDir(a)` → `ax[a]`; `planeAxes(n,…)` → `planeAxes(ax,n,…)`. The `colorFor` lambda and the center handle (camera-plane) are unchanged.

```cpp
void Gizmo::draw(Renderer& renderer, Vec3 origin, Quat rotation, Vec3 camPos) const {
    const float size = gizmoSize(camPos, origin);
    const int highlight = (axis_ >= 0) ? axis_ : hoveredAxis_;
    const bool effectiveLocal = (mode_ == GizmoMode::Scale) || (space_ == GizmoSpace::Local);
    // While dragging, draw in the frozen drag frame (startRot_) so handles match
    // the applied transform; otherwise follow the live entity rotation.
    Vec3 ax[3];
    buildBasis(effectiveLocal, (axis_ >= 0) ? startRot_ : rotation, ax);

    auto colorFor = [&](int id) -> Vec3 {
        Vec3 c;
        if (id == kCenterHandle) c = Vec3{0.85f, 0.85f, 0.85f};  // center: gray
        else if (id >= 3)        c = axisColor(id - 3);          // planar: its normal-axis color
        else                     c = axisColor(id);
        if (id == highlight) {  // brighten the hovered/active handle
            return Vec3{c.x + 0.4f > 1.0f ? 1.0f : c.x + 0.4f,
                        c.y + 0.4f > 1.0f ? 1.0f : c.y + 0.4f,
                        c.z + 0.4f > 1.0f ? 1.0f : c.z + 0.4f};
        }
        return c * 0.55f;  // dim the others
    };

    if (mode_ == GizmoMode::Rotate) {
        for (int a = 0; a < 3; ++a) drawRing(renderer, origin, ax[a], size, colorFor(a));
        return;
    }

    // Axis handles (translate arrows / scale boxes).
    for (int a = 0; a < 3; ++a) {
        const Vec3 tip = origin + ax[a] * size;
        renderer.drawLineOverlayThick(origin, tip, colorFor(a));
        if (mode_ == GizmoMode::Scale)          drawBox(renderer, tip, size * 0.08f, colorFor(a));
        else if (mode_ == GizmoMode::Translate) drawArrowhead(renderer, tip, ax[a], size, colorFor(a));
    }

    if (mode_ != GizmoMode::Translate) return;

    // Planar move handles: translucent filled quads at the inner corner, anchored
    // at the intersection (origin) — Unity-style.
    const float reach = kPlaneReach * size;
    for (int n = 0; n < 3; ++n) {
        Vec3 u, v;
        planeAxes(ax, n, u, v);
        const Vec3 c10 = origin + u * reach;
        const Vec3 c11 = origin + u * reach + v * reach;
        const Vec3 c01 = origin + v * reach;
        const Vec3 col = colorFor(3 + n);
        renderer.drawTriOverlay(origin, c10, c11, col);
        renderer.drawTriOverlay(origin, c11, c01, col);
        // Outline only the two OUTER edges (the L away from the origin).
        renderer.drawLineOverlayThick(c10, c11, col);
        renderer.drawLineOverlayThick(c01, c11, col);
    }

    // Center free-move handle: a translucent camera-facing quad at the origin,
    // with a gray outline so it reads against the scene.
    Vec3 right, up;
    cameraBasis(origin, camPos, right, up);
    const float h = size * kCenterHalf;
    const Vec3 q00 = origin - right * h - up * h;
    const Vec3 q10 = origin + right * h - up * h;
    const Vec3 q11 = origin + right * h + up * h;
    const Vec3 q01 = origin - right * h + up * h;
    const Vec3 cc = colorFor(kCenterHandle);
    renderer.drawTriOverlay(q00, q10, q11, cc);
    renderer.drawTriOverlay(q00, q11, q01, cc);
    renderer.drawLineOverlayThick(q00, q10, cc);
    renderer.drawLineOverlayThick(q10, q11, cc);
    renderer.drawLineOverlayThick(q11, q01, cc);
    renderer.drawLineOverlayThick(q01, q00, cc);
}
```

- [ ] **Step 8: Sandbox — add the `X` key toggle**

In `games/11-sandbox/main.cpp`, find the mode keys (currently `main.cpp:491-493`):

```cpp
            if (input.keyPressed(GLFW_KEY_W)) gizmo.setMode(iron::GizmoMode::Translate);
            if (input.keyPressed(GLFW_KEY_E)) gizmo.setMode(iron::GizmoMode::Rotate);
            if (input.keyPressed(GLFW_KEY_R)) gizmo.setMode(iron::GizmoMode::Scale);
```

Add the space toggle right after:

```cpp
            if (input.keyPressed(GLFW_KEY_W)) gizmo.setMode(iron::GizmoMode::Translate);
            if (input.keyPressed(GLFW_KEY_E)) gizmo.setMode(iron::GizmoMode::Rotate);
            if (input.keyPressed(GLFW_KEY_R)) gizmo.setMode(iron::GizmoMode::Scale);
            if (input.keyPressed(GLFW_KEY_X)) gizmo.toggleSpace();
```

- [ ] **Step 9: Sandbox — pass the entity rotation to `gizmo.draw`**

In `games/11-sandbox/main.cpp`, find the draw call (currently `main.cpp:626-627`):

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex), cam.position);
```

Add the rotation argument:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex),
                       scene.entities[selectedIndex].rotation, cam.position);
```

- [ ] **Step 10: Build**

Run: `cmake --build build-vk -j`
Expected: builds clean, 0 errors. (If the compiler flags an unused `worldAabb`, ignore — it's still used for picking at `main.cpp:517-523`.)

- [ ] **Step 11: Verify the existing tests still pass**

Run: `ctest --test-dir build-vk --output-on-failure`
Expected: all 46 pass (gizmo isn't unit-tested; regression guard for the rest of `ironcore`).

- [ ] **Step 12: Commit**

```bash
git add engine/editor/Gizmo.h engine/editor/Gizmo.cpp games/11-sandbox/main.cpp
git commit -m "M35: gizmo local/world space toggle (oriented handle basis)"
```

---

## Task 3: Inspector World/Local toggle + host wiring

`SceneInspector::draw` takes `GizmoSpace&` and shows a `World | Local` toggle at the top. `GizmoSpace` lives in the same `ironcore_editor` module (`Gizmo.h`), so this adds no new cross-module coupling. The host treats the gizmo as the single source of truth: read `gizmo.space()` into a local, pass it by reference (the Inspector may flip it), then write it back via `gizmo.setSpace()` (a no-op mid-drag).

**Files:**
- Modify: `engine/editor/SceneInspector.h`
- Modify: `engine/editor/SceneInspector.cpp`
- Modify: `games/11-sandbox/main.cpp:536-537`

- [ ] **Step 1: Update the Inspector header signature**

Replace the whole of `engine/editor/SceneInspector.h` with:

```cpp
#pragma once

#include "editor/Gizmo.h"  // for GizmoSpace

namespace iron {

struct SceneEntity;

// Details panel for a single entity: transform (position, euler rotation,
// scale) and material scalars (emissive, uvScale, reflectivity). Mesh info is
// shown read-only. Mutates the entity in place. Also hosts the gizmo World/Local
// space toggle (mirrors the `X` key), which it reads from and writes to `space`.
class SceneInspector {
public:
    // Returns true if any entity field changed this frame.
    bool draw(SceneEntity& entity, GizmoSpace& space);
};

}  // namespace iron
```

- [ ] **Step 2: Add the toggle UI + new signature (`SceneInspector.cpp`)**

In `engine/editor/SceneInspector.cpp`, change the function signature:

```cpp
bool SceneInspector::draw(SceneEntity& e) {
```

to:

```cpp
bool SceneInspector::draw(SceneEntity& e, GizmoSpace& space) {
```

Then, find:

```cpp
    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    ImGui::SeparatorText("Transform");
```

and insert the space toggle between the name line and the Transform section:

```cpp
    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    // Gizmo space toggle (mirrors the X key). Scale handles are always local
    // regardless of this setting.
    ImGui::SeparatorText("Gizmo Space");
    int spaceInt = (space == GizmoSpace::Local) ? 1 : 0;
    bool spaceChanged = false;
    spaceChanged |= ImGui::RadioButton("World", &spaceInt, 0);
    ImGui::SameLine();
    spaceChanged |= ImGui::RadioButton("Local", &spaceInt, 1);
    if (spaceChanged)
        space = (spaceInt == 1) ? GizmoSpace::Local : GizmoSpace::World;

    ImGui::SeparatorText("Transform");
```

> The space change is intentionally **not** folded into the `changed` return value — that flag means "an entity field changed". The host applies the space every frame regardless (Step 3).

- [ ] **Step 3: Host wiring — make the gizmo the source of truth**

In `games/11-sandbox/main.cpp`, find the inspector call (currently `main.cpp:536-537`):

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            inspector.draw(scene.entities[selectedIndex]);
```

Replace it with read-modify-write through the gizmo:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            iron::GizmoSpace sp = gizmo.space();
            inspector.draw(scene.entities[selectedIndex], sp);
            gizmo.setSpace(sp);  // Inspector may flip it; setSpace is a no-op mid-drag
        }
```

- [ ] **Step 4: Build**

Run: `cmake --build build-vk -j`
Expected: builds clean, 0 errors.

- [ ] **Step 5: Verify the existing tests still pass**

Run: `ctest --test-dir build-vk --output-on-failure`
Expected: all 46 pass.

- [ ] **Step 6: Commit**

```bash
git add engine/editor/SceneInspector.h engine/editor/SceneInspector.cpp games/11-sandbox/main.cpp
git commit -m "M35: Inspector World/Local gizmo-space toggle + host wiring"
```

---

## Task 4: Docs — `docs/engine/editor.md`

Document the World/Local toggle and the now-oriented selection outline. (Anchors
below match the real file: the `### SceneInspector` block, the host-contract
snippet, the `### Gizmo controls` table, and the `### Known v1 gizmo limitations`
list.)

**Files:**
- Modify: `docs/engine/editor.md`

- [ ] **Step 1: Update the `SceneInspector` doc block (signature + note)**

Find (currently `docs/engine/editor.md:60-65`):

```markdown
```cpp
// engine/editor/SceneInspector.h
bool draw(SceneEntity& entity);
```

Edits the selected entity in place. Returns `true` if any field changed.
```

Replace with:

```markdown
```cpp
// engine/editor/SceneInspector.h
bool draw(SceneEntity& entity, GizmoSpace& space);
```

Edits the selected entity in place. Returns `true` if any field changed. Also
hosts the gizmo **World/Local** space toggle at the top of the panel (mirrors the
**X** key), reading from and writing to `space`.
```

- [ ] **Step 2: Update the host-contract snippet to the new wiring**

Find (currently `docs/engine/editor.md:112-113`):

```markdown
   if (selectedIdx >= 0)
       inspector.draw(scene.entities[selectedIdx]);
```

Replace with:

```markdown
   if (selectedIdx >= 0) {
       iron::GizmoSpace sp = gizmo.space();   // gizmo is the source of truth
       inspector.draw(scene.entities[selectedIdx], sp);
       gizmo.setSpace(sp);                    // Inspector may flip it; no-op mid-drag
   }
```

- [ ] **Step 3: Add the `X` key to the Gizmo controls table**

Find (currently `docs/engine/editor.md:190-195`):

```markdown
| Key | Action |
| --- | ------ |
| **W** | Translate mode |
| **E** | Rotate mode |
| **R** | Scale mode |
| Hold **RMB** | Free-fly look (gizmo keys suppressed while flying) |
```

Replace with:

```markdown
| Key | Action |
| --- | ------ |
| **W** | Translate mode |
| **E** | Rotate mode |
| **R** | Scale mode |
| **X** | Toggle World / Local space (also in the Inspector) |
| Hold **RMB** | Free-fly look (gizmo keys suppressed while flying) |
```

- [ ] **Step 4: Update the "world-axis handles" description**

Find (currently `docs/engine/editor.md:197-200`):

```markdown
When an entity is selected the `iron::Gizmo` controller
(`editor/Gizmo.h`, in `ironcore_editor`) draws world-axis handles for the
active mode. The gizmo is distance-scaled to maintain a constant apparent
screen size regardless of how far the camera is from the entity.
```

Replace with:

```markdown
When an entity is selected the `iron::Gizmo` controller
(`editor/Gizmo.h`, in `ironcore_editor`) draws handles for the active mode in
either **World** (canonical X/Y/Z) or **Local** (aligned to the entity's
rotation) space — toggled with **X** or the Inspector's *Gizmo Space* toggle.
Scale handles are always per-local-axis regardless of the setting (matching
Unreal/Unity). The gizmo is distance-scaled to maintain a constant apparent
screen size regardless of how far the camera is from the entity.
```

- [ ] **Step 5: Document the oriented outline (after the "Dragging any handle" paragraph)**

Find (currently `docs/engine/editor.md:221-223`):

```markdown
Dragging any handle transforms the entity live by updating its position,
rotation, or scale in the in-memory `SceneFile`. The same mutations are
immediately reflected in the Inspector. Press **Save Scene** to persist them.
```

Replace with (append a Selection outline paragraph):

```markdown
Dragging any handle transforms the entity live by updating its position,
rotation, or scale in the in-memory `SceneFile`. The same mutations are
immediately reflected in the Inspector. Press **Save Scene** to persist them.

The selected entity also gets an always-on-top **oriented bounding box**
(selection-orange): the sandbox transforms each corner of the entity's local
bounds by the model matrix every frame, so the box rotates and scales **with**
the object instead of a world-axis AABB that just grows on rotation. It hugs the
mesh's local *bounds*, not its vertices — a true vertex silhouette
(stencil/edge-detect pass) is a later milestone.
```

- [ ] **Step 6: Refresh the "World-axis only" limitation bullet**

Find (currently `docs/engine/editor.md:244-246`):

```markdown
- **World-axis only.** Handles align to world X/Y/Z (no local-space gizmos);
  the planar handles cover the world planes only. No screen-space center
  free-move, snapping, multi-select, or undo.
```

Replace with:

```markdown
- **Bounds-box outline, no vertex silhouette.** The selection outline is the
  oriented local-bounds box, not a true mesh silhouette (a stencil/edge-detect
  outline is a later milestone). No snapping, multi-select, or undo.
```

- [ ] **Step 7: Commit**

```bash
git add docs/engine/editor.md
git commit -m "docs(M35): document gizmo World/Local toggle + oriented outline"
```

---

## Task 5: User visual verification (the acceptance gate)

The gizmo and outline are not unit-testable; the user verifies in the running sandbox. **Do not** claim M35 complete until the user confirms.

- [ ] **Step 1: Hand off for visual verification**

Ask the user to run the sandbox (`games/11-sandbox`) and confirm each:

1. **Outline rotates:** Select an object (cube/helmet), rotate it — the orange outline **rotates with it** (no longer just grows/shrinks).
2. **Local handles:** Press **X** (or the Inspector *Gizmo Space → Local*) — translate + rotate handles **align to the object's orientation**. Toggle back to **World** — axes are world-aligned again.
3. **Scale always local:** In Scale mode (**R**), handles align to the object in **both** World and Local — and dragging local-X grows `scale.x`.
4. **Inspector mirrors state:** The Inspector's World/Local toggle reflects the current space, and the **X** key flips it; the toggle is ignored mid-drag.
5. **World unchanged:** In World mode everything behaves exactly as before M35 (no regressions in translate/rotate/scale/planar/center handles).

- [ ] **Step 2: On approval — finish the branch**

REQUIRED SUB-SKILL: Use superpowers:finishing-a-development-branch to open/merge the PR, then update memory (`iron-core-engine-progress.md`, `iron-core-engine-roadmap.md`) with M35 merged and the next milestone (stencil-buffer vertex silhouette outline).

---

## Self-Review (performed against the spec)

- **Spec coverage:** Part 1 (oriented outline) → Task 1. Part 2 (basis, scale-always-local, drag-stability via `startRot_`, oriented translate/rotate/planar/scale, `setSpace`/`toggleSpace`/`space`, `draw` rotation arg) → Task 2. Part 3 (X key, Inspector toggle, host single-source-of-truth wiring) → Tasks 2+3. Docs → Task 4 (anchors corrected to match the real `editor.md`). Testing/verification model → per-task build+ctest gates and Task 5. ✓ all covered.
- **Type/signature consistency:** `GizmoSpace { World, Local }`; `buildBasis(bool, const Quat&, Vec3[3])`; `pickHandle(GizmoMode, const Ray&, Vec3, const Vec3[3], float)`; `planeHandleHit(..., const Vec3[3], int, float)`; `planeAxes(const Vec3[3], int, Vec3&, Vec3&)`; `ringAngle(const Ray&, Vec3, const Vec3[3], int, float&)`; `draw(Renderer&, Vec3, Quat, Vec3)`; `SceneInspector::draw(SceneEntity&, GizmoSpace&)` — used identically at every call site in the plan. ✓
- **Placeholder scan:** every code step shows complete code; no TBD/"handle edge cases"/"similar to". ✓
- **Build-green per task:** each signature change moves with its call sites inside the same task (Task 2 carries the `gizmo.draw` site; Task 3 carries the `inspector.draw` site). ✓
