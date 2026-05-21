# Strandbound M4 — "Tie & Cut" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add raycasting math to the Iron Core Engine and a game-side `RopeTool`, so the player can place anchors on surfaces and tie / cut ropes between them.

**Architecture:** Builds on the completed M1–M3 engine. M4 adds: pure raycasting geometry (`Ray`, `Aabb`, ray-vs-box / ray-vs-sphere) to `engine/math/`, an `aimRay()` accessor on `FirstPersonController`, and a mouse-button edge query on `Input`. The Strandbound game splits into `main.cpp` (setup + wiring) and a new `RopeTool` that owns anchors, ropes, and the place/tie/cut interaction.

**Tech Stack:** C++23, CMake, MSVC, OpenGL 3.3 (via the existing RHI). No new third-party dependencies.

**Conventions:**
- Namespace `iron` for engine code. Engine headers included relative to `engine/`. `Mat4` column-major.
- Build: `cmake -S . -B build` then `cmake --build build`.
- Tests (MSVC multi-config): `ctest --test-dir build -C Debug --output-on-failure`.
- Commit after every task; commit messages end with the `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.
- Spec: `docs/superpowers/specs/2026-05-21-strandbound-m4-tie-and-cut-design.md`.

---

## File Structure

**Created by this plan:**

```
engine/math/Ray.h                       Ray struct + ray-vs-sphere + ray-vs-box
engine/math/Aabb.h                      Aabb struct
games/02-strandbound/RopeTool.h, .cpp   anchors, ropes, place/tie/cut, drawing
tests/test_ray.cpp                      ray-intersection unit tests
docs/engine/raycasting.md               concept note
```

**Modified by this plan:**

```
engine/scene/FirstPersonController.h/.cpp  aimRay() accessor
engine/core/Input.h/.cpp                   mouseButtonPressed edge query
games/02-strandbound/main.cpp              split: setup + wiring; drives RopeTool
games/02-strandbound/CMakeLists.txt        add RopeTool.cpp to the executable
tests/CMakeLists.txt                       register test_ray
tests/test_first_person_controller.cpp     aimRay test
```

---

## Task 1: Ray and ray-vs-sphere

**Files:**
- Create: `tests/test_ray.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `engine/math/Ray.h`

- [ ] **Step 1: Write the failing test `tests/test_ray.cpp`**

```cpp
#include "test_framework.h"
#include "math/Ray.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Direct hit: ray along +Z, sphere ahead at z = 10, radius 1.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        const bool hit = intersectRaySphere(ray, Vec3{0.0f, 0.0f, 10.0f}, 1.0f, t);
        CHECK(hit);
        CHECK_NEAR(t, 9.0f);  // nearest surface is at z = 9
    }

    // Miss: ray points away from the sphere.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        float t = -1.0f;
        CHECK(!intersectRaySphere(ray, Vec3{0.0f, 0.0f, 10.0f}, 1.0f, t));
    }

    // Miss: ray passes to the side of the sphere.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        CHECK(!intersectRaySphere(ray, Vec3{5.0f, 0.0f, 10.0f}, 1.0f, t));
    }

    // Origin inside the sphere: hit reported at t = 0.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        const bool hit = intersectRaySphere(ray, Vec3{0.0f, 0.0f, 0.0f}, 5.0f, t);
        CHECK(hit);
        CHECK_NEAR(t, 0.0f);
    }

    // Glancing hit near the rim still registers.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        float t = -1.0f;
        CHECK(intersectRaySphere(ray, Vec3{0.99f, 0.0f, 10.0f}, 1.0f, t));
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

Read `tests/CMakeLists.txt`. Add after the existing `iron_add_test(test_verlet test_verlet.cpp)` line:

```cmake
iron_add_test(test_ray test_ray.cpp)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: build FAILS — `math/Ray.h` does not exist.

- [ ] **Step 4: Write `engine/math/Ray.h`**

```cpp
#pragma once

#include "math/Vec.h"

#include <cmath>

namespace iron {

// A ray for picking and line-of-sight queries. `direction` is expected to be
// unit length.
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Ray vs sphere. If the ray hits the sphere at or in front of its origin,
// sets `outT` to the distance along the ray of the nearest intersection and
// returns true. A ray whose origin is inside the sphere reports t = 0.
inline bool intersectRaySphere(const Ray& ray, Vec3 center, float radius,
                               float& outT) {
    const Vec3 m = ray.origin - center;
    const float b = dot(m, ray.direction);
    const float c = dot(m, m) - radius * radius;

    // Origin outside the sphere (c > 0) and the ray pointing away (b > 0): miss.
    if (c > 0.0f && b > 0.0f) {
        return false;
    }
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return false;  // the ray misses the sphere entirely
    }

    float t = -b - std::sqrt(discriminant);
    if (t < 0.0f) {
        t = 0.0f;  // the ray started inside the sphere
    }
    outT = t;
    return true;
}

} // namespace iron
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 7 tests pass, including `test_ray`.

- [ ] **Step 6: Commit**

```bash
git add engine/math/Ray.h tests/test_ray.cpp tests/CMakeLists.txt
git commit -m "Add Ray and ray-vs-sphere intersection with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 2: Aabb and ray-vs-box

**Files:**
- Create: `engine/math/Aabb.h`
- Modify: `engine/math/Ray.h`
- Modify: `tests/test_ray.cpp`

- [ ] **Step 1: Write `engine/math/Aabb.h`**

```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// An axis-aligned bounding box, defined by its minimum and maximum corners.
struct Aabb {
    Vec3 min;
    Vec3 max;
};

} // namespace iron
```

- [ ] **Step 2: Add `intersectRayAabb` to `engine/math/Ray.h`**

Add `#include "math/Aabb.h"` to `Ray.h` (after the `#include "math/Vec.h"` line). Then add this function inside `namespace iron`, after `intersectRaySphere`:

```cpp
// Ray vs axis-aligned box (the slab method). If the ray hits the box at or in
// front of its origin, sets `outT` to the entry distance and returns true. A
// ray whose origin is inside the box reports t = 0.
inline bool intersectRayAabb(const Ray& ray, const Aabb& box, float& outT) {
    float tMin = 0.0f;
    float tMax = 1e30f;

    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float boxMin[3] = {box.min.x, box.min.y, box.min.z};
    const float boxMax[3] = {box.max.x, box.max.y, box.max.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < 1e-8f) {
            // Ray parallel to this slab: miss if the origin is outside it.
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) {
                return false;
            }
        } else {
            const float inv = 1.0f / dir[axis];
            float t1 = (boxMin[axis] - origin[axis]) * inv;
            float t2 = (boxMax[axis] - origin[axis]) * inv;
            if (t1 > t2) {
                const float tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if (t1 > tMin) tMin = t1;
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) {
                return false;  // the slabs do not overlap: miss
            }
        }
    }
    outT = tMin;
    return true;
}
```

- [ ] **Step 3: Add box tests to `tests/test_ray.cpp`**

Add these five test blocks to `tests/test_ray.cpp`, immediately before the
`return iron_test_result();` line:

```cpp
    // intersectRayAabb: direct hit on a box ahead along +Z.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        const bool hit = intersectRayAabb(ray, box, t);
        CHECK(hit);
        CHECK_NEAR(t, 5.0f);  // entry face is at z = 5
    }

    // Miss: ray points away from the box.
    {
        Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }

    // Miss: ray passes well beside the box.
    {
        Ray ray{Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }

    // Origin inside the box: hit reported at t = 0.
    {
        Ray ray{Vec3{0.0f, 0.0f, 6.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        const bool hit = intersectRayAabb(ray, box, t);
        CHECK(hit);
        CHECK_NEAR(t, 0.0f);
    }

    // Ray parallel to a slab but outside it: miss.
    {
        Ray ray{Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};  // y=5, box y in [-1,1]
        const Aabb box{Vec3{-1.0f, -1.0f, 5.0f}, Vec3{1.0f, 1.0f, 7.0f}};
        float t = -1.0f;
        CHECK(!intersectRayAabb(ray, box, t));
    }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 7 tests pass; `test_ray` now covers spheres and boxes.

- [ ] **Step 5: Commit**

```bash
git add engine/math/Aabb.h engine/math/Ray.h tests/test_ray.cpp
git commit -m "Add Aabb and ray-vs-box intersection with tests"
```
(plus the `Co-Authored-By` trailer)

---

## Task 3: FirstPersonController::aimRay()

**Files:**
- Modify: `tests/test_first_person_controller.cpp`
- Modify: `engine/scene/FirstPersonController.h`
- Modify: `engine/scene/FirstPersonController.cpp`

- [ ] **Step 1: Add the failing test**

In `tests/test_first_person_controller.cpp`, add `#include "math/Ray.h"` after
the existing `#include "math/Vec.h"` line. Then add this test block
immediately before the `return iron_test_result();` line:

```cpp
    // aimRay starts at the eye position and points along the look direction.
    {
        FirstPersonController c;
        c.setPosition(Vec3{2.0f, 0.0f, 5.0f});
        c.setEyeHeight(1.5f);
        // Default yaw = 0, pitch = 0 looks toward -Z.
        Ray r = c.aimRay();
        CHECK_NEAR(r.origin.x, 2.0f);
        CHECK_NEAR(r.origin.y, 1.5f);
        CHECK_NEAR(r.origin.z, 5.0f);
        CHECK_NEAR(r.direction.x, 0.0f);
        CHECK_NEAR(r.direction.y, 0.0f);
        CHECK_NEAR(r.direction.z, -1.0f);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: build FAILS — `FirstPersonController` has no member `aimRay`.

- [ ] **Step 3: Declare `aimRay` in `FirstPersonController.h`**

In `engine/scene/FirstPersonController.h`, add `#include "math/Ray.h"` after
the existing `#include "math/Mat4.h"` line. Then add this declaration in the
`public:` section, after the `viewMatrix()` / `eyePosition()` declarations:

```cpp
    // The ray from the eye along the current look direction — the player's aim.
    Ray aimRay() const;
```

- [ ] **Step 4: Define `aimRay` in `FirstPersonController.cpp`**

In `engine/scene/FirstPersonController.cpp`, add this definition after the
`viewMatrix()` definition:

```cpp
Ray FirstPersonController::aimRay() const {
    return Ray{eyePosition(), forwardDir()};
}
```

(`forwardDir()` is an existing private helper; `aimRay` is a member, so it can
call it. `forwardDir()` already returns a unit-length direction.)

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -C Debug --output-on-failure`
Expected: all 7 tests pass.

- [ ] **Step 6: Commit**

```bash
git add engine/scene/FirstPersonController.h engine/scene/FirstPersonController.cpp tests/test_first_person_controller.cpp
git commit -m "Add FirstPersonController::aimRay()"
```
(plus the `Co-Authored-By` trailer)

---

## Task 4: Input::mouseButtonPressed

`Input` already tracks per-key previous/current state for `keyPressed`. It has
only a level-state `mouseButtonDown` for mouse buttons. This task adds the same
previous/current tracking for mouse buttons and a `mouseButtonPressed` edge
query.

**Files:**
- Modify: `engine/core/Input.h`
- Modify: `engine/core/Input.cpp`

Read both files first; make precise additions.

- [ ] **Step 1: Add the mouse-button state to `Input.h`**

In `engine/core/Input.h`:

(a) Next to the existing `static constexpr int kKeyCount = 350;`, add:

```cpp
    static constexpr int kMouseButtonCount = 8;  // GLFW_MOUSE_BUTTON_LAST is 7
```

(b) In the `public:` section, next to the `mouseButtonDown` declaration, add:

```cpp
    bool mouseButtonPressed(int button) const;  // went down this frame
```

(c) In the `private:` section, next to the `bool current_[kKeyCount]` /
`bool previous_[kKeyCount]` arrays, add:

```cpp
    bool currentMouse_[kMouseButtonCount] = {};
    bool previousMouse_[kMouseButtonCount] = {};
```

- [ ] **Step 2: Update `Input::update()` in `Input.cpp`**

In `engine/core/Input.cpp`, inside `update()`, after the existing loop that
polls the keyboard keys (and before or after the cursor-position polling —
order does not matter), add a loop that polls the mouse buttons:

```cpp
    for (int button = 0; button < kMouseButtonCount; ++button) {
        previousMouse_[button] = currentMouse_[button];
        currentMouse_[button] =
            glfwGetMouseButton(window_, button) == GLFW_PRESS;
    }
```

- [ ] **Step 3: Define `mouseButtonPressed` in `Input.cpp`**

Add this definition after the existing `mouseButtonDown` definition:

```cpp
bool Input::mouseButtonPressed(int button) const {
    return button >= 0 && button < kMouseButtonCount
           && currentMouse_[button] && !previousMouse_[button];
}
```

- [ ] **Step 4: Build**

Run: `cmake --build build`
Expected: builds clean — `ironcore`, both games, and all test executables.

- [ ] **Step 5: Commit**

```bash
git add engine/core/Input.h engine/core/Input.cpp
git commit -m "Add Input::mouseButtonPressed edge query"
```
(plus the `Co-Authored-By` trailer)

---

## Task 5: The RopeTool

A game-side class owning anchors, ropes, and the place / tie / cut
interaction. This task creates the class and adds it to the build; Task 6
wires it into the game.

**Files:**
- Create: `games/02-strandbound/RopeTool.h`
- Create: `games/02-strandbound/RopeTool.cpp`
- Modify: `games/02-strandbound/CMakeLists.txt`

- [ ] **Step 1: Write `games/02-strandbound/RopeTool.h`**

```cpp
#pragma once

#include "math/Aabb.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <vector>

namespace iron { class Renderer; }

// The Strandbound rope tool: the player places anchor points on world
// surfaces and ties / cuts ropes between them. This is game-specific
// interaction logic, so it lives with the game rather than the engine.
class RopeTool {
public:
    // `colliders` are the static world boxes (islands, props, pole) the
    // aim ray is tested against when placing an anchor.
    explicit RopeTool(std::vector<iron::Aabb> colliders);

    // Advance one fixed step. `aim` is the player's aim ray; `playerPos` is
    // the player's feet position. The three flags are this step's input
    // edges (true only on the step the button/key went down).
    void update(const iron::Ray& aim, iron::Vec3 playerPos,
                bool placePressed, bool tiePressed, bool cutPressed,
                float dt);

    // Queue debug lines for anchors, ropes, the tying guide line, and the
    // aim marker. Call between submitting the scene and flushDebugLines.
    void draw(iron::Renderer& renderer) const;

private:
    enum class AimKind { None, Surface, Anchor, Rope };

    // Nearest anchor the aim ray passes through, or -1.
    int pickAnchor(const iron::Ray& aim) const;
    // Nearest rope the aim ray passes through, or -1; sets outPoint to the
    // rope point that was hit.
    int pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const;
    // Nearest surface hit of the aim ray against the colliders.
    bool pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const;
    // Recompute what the aim ray currently targets (for the aim marker).
    void refreshAimTarget(const iron::Ray& aim);

    std::vector<iron::Aabb> colliders_;
    std::vector<iron::Vec3> anchors_;
    std::vector<iron::Rope> ropes_;

    int tyingFromAnchor_ = -1;        // anchor index being tied from, or -1
    iron::Vec3 playerPos_{};          // cached, for drawing the tying guide

    AimKind aimKind_ = AimKind::None;
    iron::Vec3 aimPoint_{};
};
```

- [ ] **Step 2: Write `games/02-strandbound/RopeTool.cpp`**

```cpp
#include "RopeTool.h"

#include "physics/VerletPoint.h"
#include "render/Renderer.h"

#include <cstddef>
#include <utility>

namespace {
constexpr float kAnchorPickRadius = 0.5f;   // anchors picked as spheres
constexpr float kRopePickRadius = 0.3f;     // rope points picked as spheres
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;       // rope length vs. anchor span
constexpr float kMarkerSize = 0.25f;
}  // namespace

RopeTool::RopeTool(std::vector<iron::Aabb> colliders)
    : colliders_(std::move(colliders)) {}

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
        if (iron::intersectRayAabb(aim, box, t) && t < bestT) {
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

    // Place: drop an anchor on the nearest surface the aim ray hits.
    if (placePressed) {
        iron::Vec3 hit;
        if (pickSurface(aim, hit)) {
            anchors_.push_back(hit);
        }
    }

    // Tie: first click picks the start anchor; second click (a different
    // anchor) creates a rope spanning the two.
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

    // Cut: remove the whole rope the aim ray hits.
    if (cutPressed) {
        iron::Vec3 unused;
        const int rope = pickRope(aim, unused);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin()
                         + static_cast<std::ptrdiff_t>(rope));
        }
    }

    // Advance every rope's Verlet simulation.
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }

    refreshAimTarget(aim);
}

void RopeTool::draw(iron::Renderer& renderer) const {
    const iron::Vec3 anchorColor{0.95f, 0.8f, 0.2f};
    const iron::Vec3 ropeColor{0.55f, 0.35f, 0.18f};
    const iron::Vec3 guideColor{0.3f, 0.85f, 0.95f};

    // Anchors: a small three-axis cross at each.
    for (const iron::Vec3& a : anchors_) {
        const float s = kMarkerSize;
        renderer.drawLine(a - iron::Vec3{s, 0.0f, 0.0f},
                          a + iron::Vec3{s, 0.0f, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, s, 0.0f},
                          a + iron::Vec3{0.0f, s, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, 0.0f, s},
                          a + iron::Vec3{0.0f, 0.0f, s}, anchorColor);
    }

    // Ropes: one debug line per segment.
    for (const iron::Rope& r : ropes_) {
        const std::vector<iron::VerletPoint>& pts = r.points();
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            renderer.drawLine(pts[i].position, pts[i + 1].position, ropeColor);
        }
    }

    // While tying: a guide line from the start anchor to the player.
    if (tyingFromAnchor_ >= 0) {
        renderer.drawLine(anchors_[static_cast<std::size_t>(tyingFromAnchor_)],
                          playerPos_ + iron::Vec3{0.0f, 1.0f, 0.0f},
                          guideColor);
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

- [ ] **Step 3: Add `RopeTool.cpp` to `games/02-strandbound/CMakeLists.txt`**

Read `games/02-strandbound/CMakeLists.txt`. Change the `add_executable` line
so the executable also compiles `RopeTool.cpp`:

```cmake
add_executable(strandbound main.cpp RopeTool.cpp)
```

Leave the `target_link_libraries` and the `add_custom_command` asset-copy
block unchanged.

- [ ] **Step 4: Build**

Run: `cmake -S . -B build && cmake --build build`
Expected: builds clean. `RopeTool.cpp` compiles into the `strandbound`
executable. `main.cpp` does not use `RopeTool` yet — that is Task 6 — so the
class is compiled but unused for now, which is fine.

- [ ] **Step 5: Commit**

```bash
git add games/02-strandbound/RopeTool.h games/02-strandbound/RopeTool.cpp games/02-strandbound/CMakeLists.txt
git commit -m "Add RopeTool: anchor placement and rope tie/cut"
```
(plus the `Co-Authored-By` trailer)

---

## Task 6: Wire the RopeTool into the game

Replace M3's single hardcoded rope with the player-driven `RopeTool`. The pole
stays as a scene object (a handy thing to tie to).

**Files:**
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Replace `games/02-strandbound/main.cpp` with the M4 version**

Replace the entire file with:

```cpp
#include "RopeTool.h"

#include "core/Application.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Aabb.h"
#include "math/Transform.h"
#include "render/Light.h"
#include "render/backends/opengl/OpenGLRenderer.h"
#include "scene/FirstPersonController.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <GLFW/glfw3.h>

#include <vector>

namespace {

// Vertex shader: MVP transform; passes the world-space normal and UV through.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vNormal;
out vec2 vUV;

void main() {
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

// Fragment shader: Lambert diffuse from one directional light + ambient.
const char* kFragmentShader = R"(#version 330 core
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

// Vertical field of view for the camera: 60 degrees.
constexpr float kFovYRadians = 3.14159265f / 3.0f;

// One solid box in the world: its centre and full size.
struct BoxDef {
    iron::Vec3 center;
    iron::Vec3 size;
};

// A unit cube scaled and translated into place.
iron::RenderObject makeBox(const BoxDef& def, iron::MeshHandle mesh,
                           iron::TextureHandle texture) {
    iron::RenderObject obj;
    obj.transform = iron::translation(def.center) * iron::scaling(def.size);
    obj.mesh = mesh;
    obj.texture = texture;
    return obj;
}

}  // namespace

int main() {
    iron::Application::Config config;
    config.title = "Iron Core Engine - Strandbound (M4)";
    iron::Application app(config);
    if (!app.valid()) {
        iron::Log::error("Application init failed");
        return 1;
    }

    iron::OpenGLRenderer renderer;

    const iron::MeshHandle cube = renderer.createMesh(iron::makeCube());
    const iron::ShaderHandle shader =
        renderer.createShader(kVertexShader, kFragmentShader);
    const iron::TextureHandle texture =
        renderer.loadTexture(iron::executableDir() + "/assets/crate.jpg");
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("Shader failed to compile; aborting");
        return 1;
    }

    // The solid geometry of the level: a home island, props, a far island,
    // and a pole. One BoxDef list builds both the render objects and the
    // collider list the RopeTool raycasts against.
    const BoxDef boxes[] = {
        {{0.0f, -0.5f, 0.0f},  {20.0f, 1.0f, 20.0f}},  // home island
        {{2.0f, 0.5f, -3.0f},  {1.0f, 1.0f, 1.0f}},    // prop
        {{-3.0f, 1.0f, -1.0f}, {1.0f, 2.0f, 1.0f}},    // prop (taller)
        {{-1.0f, 0.75f, 4.0f}, {1.5f, 1.5f, 1.5f}},    // prop
        {{0.0f, -0.5f, -45.0f},{18.0f, 1.0f, 18.0f}},  // far island
        {{5.0f, 2.0f, 0.0f},   {0.4f, 4.0f, 0.4f}},    // pole
    };

    iron::Scene scene;
    scene.light.direction = iron::Vec3{-0.4f, -1.0f, -0.3f};
    scene.light.color = iron::Vec3{1.0f, 0.97f, 0.9f};
    scene.light.ambient = 0.25f;

    std::vector<iron::Aabb> colliders;
    for (const BoxDef& def : boxes) {
        scene.objects.push_back(makeBox(def, cube, texture));
        const iron::Vec3 half = def.size * 0.5f;
        colliders.push_back(iron::Aabb{def.center - half, def.center + half});
    }

    iron::FirstPersonController player;
    player.setGroundHeight(0.0f);
    player.setEyeHeight(1.7f);
    player.setPosition(iron::Vec3{0.0f, 0.0f, 7.0f});
    player.setMoveSpeed(6.0f);
    player.setMouseSensitivity(0.0025f);

    RopeTool ropeTool(colliders);

    app.window().setCursorCaptured(true);

    const float aspect = static_cast<float>(app.window().width()) /
                         static_cast<float>(app.window().height());
    const iron::Mat4 projection =
        iron::perspective(kFovYRadians, aspect, 0.1f, 200.0f);

    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        iron::ControllerInput ci;
        if (input.keyDown(GLFW_KEY_W)) ci.forward += 1.0f;
        if (input.keyDown(GLFW_KEY_S)) ci.forward -= 1.0f;
        if (input.keyDown(GLFW_KEY_D)) ci.strafe += 1.0f;
        if (input.keyDown(GLFW_KEY_A)) ci.strafe -= 1.0f;
        ci.mouseDX = static_cast<float>(input.mouseDeltaX());
        ci.mouseDY = static_cast<float>(input.mouseDeltaY());
        player.update(ci, time.deltaSeconds);

        // Rope tool: right-click places an anchor, left-click ties, C cuts.
        const bool place = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
        const bool tie = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
        const bool cut = input.keyPressed(GLFW_KEY_C);
        ropeTool.update(player.aimRay(), player.position(), place, tie, cut,
                        time.deltaSeconds);
    });

    app.setRender([&] {
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

        ropeTool.draw(renderer);
        renderer.flushDebugLines(view, projection);

        renderer.endFrame();
    });

    app.run();
    return 0;
}
```

- [ ] **Step 2: Build**

Run: `cmake --build build`
Expected: builds clean; `strandbound.exe` is produced.

- [ ] **Step 3: Run — milestone acceptance check**

Run: `build/games/02-strandbound/Debug/strandbound.exe`

Expected — **this is the M4 acceptance check**:
- A first-person lit island, as in M3, with a pole.
- A world-space aim marker (a small cross) appears where the player is
  looking, white on bare surfaces.
- **Right-click** on a surface places a yellow-cross anchor there.
- **Left-click** on one anchor, then left-click on another, creates a brown
  rope that spans the two anchors and hangs under gravity. A cyan guide line
  is shown from the first anchor to the player between the two clicks.
- **C** while aiming at a rope removes it.
- The aim marker turns yellow over anchors and red over ropes.
- The player can build a small rope web and tear it down. `Escape` quits.

> Visual verification is done by the controller / user, not an implementer
> subagent. If running the game blocks, just confirm it builds and launches
> without an immediate crash.

- [ ] **Step 4: Commit**

```bash
git add games/02-strandbound/main.cpp
git commit -m "MILESTONE M4: player-driven anchors and rope tie/cut"
```
(plus the `Co-Authored-By` trailer)

---

## Task 7: Raycasting concept note

**Files:**
- Create: `docs/engine/raycasting.md`

- [ ] **Step 1: Write `docs/engine/raycasting.md`**

Create the file with exactly this content (it is plain Markdown prose — no
code fences):

```
# Raycasting

A **ray** is a half-line: an origin and a direction. Raycasting asks "what does
this ray hit, and how far away?" — the engine's tool for *picking* (what is the
player aiming at?) and line-of-sight queries.

## The Ray

`iron::Ray` is just `{ origin, direction }`, with `direction` kept unit length.
The player's aim ray comes from `FirstPersonController::aimRay()` — the eye
position, pointing where the camera looks.

## Intersection tests

The engine provides two pure intersection functions. Each reports whether the
ray hits and, on a hit, the distance `t` along the ray to the nearest contact:

- **Ray vs sphere** — used to pick small round targets: rope anchors, and rope
  points when cutting. It solves the quadratic for where the ray meets the
  sphere surface.
- **Ray vs box** (axis-aligned) — used to find the surface point when placing
  an anchor. It uses the **slab method**: a box is the overlap of three slabs
  (one per axis); the ray is inside the box only where all three slab
  intervals overlap.

Both clamp `t` to be non-negative — a hit is always at or in front of the ray
origin, never behind it.

## Nearest hit

A single ray often crosses several candidates. The caller tests them all and
keeps the smallest `t` — the first thing the ray reaches. That is how the rope
tool decides which anchor you are pointing at when several line up.

## Why not a physics world?

The engine deliberately stops at intersection *functions*. It does not keep a
registry of colliders or a `raycast(scene)` query. With a few dozen objects the
game iterating its own list is simpler and fast enough; a managed physics world
earns its place only at a much larger scale.

Related: [[rope-physics]], [[render-pipeline]]
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/raycasting.md
git commit -m "Add raycasting concept note"
```
(plus the `Co-Authored-By` trailer)

---

## Done

The engine has reusable raycasting geometry (`Ray`, `Aabb`, ray-vs-sphere,
ray-vs-box) and the player can drive ropes: place anchors on surfaces, tie
ropes between them, and cut them. M5 (bridge the gap) builds on this and gets
its own spec and plan.
