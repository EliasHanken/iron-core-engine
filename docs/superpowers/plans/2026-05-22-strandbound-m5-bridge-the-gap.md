# Strandbound M5 "Bridge the Gap" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Strandbound sandbox into a playable demo — a real gap that resets the player on a fall, a tightrope-balance rope traversal, and a win on reaching the far island.

**Architecture:** One engine addition (`Hud::setSize`). A new game-side `RopeWalker` (pure helpers + a headless state machine) owns the tightrope traversal — mount, lean meter, movement, camera roll, fall/dismount/win. `main.cpp` gains a player state (`Walking`/`Traversing`/`Won`), a per-frame footing query against the island colliders, respawn, and the lean-meter + win HUD.

**Tech Stack:** C++23, OpenGL 3.3 via the engine RHI, CMake, MSVC, the project's custom CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-22-strandbound-m5-bridge-the-gap-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `engine/ui/Hud.h/.cpp` (modified) | Adds `setSize(HudId, Vec2)`. |
| `games/02-strandbound/RopeWalker.h/.cpp` (new) | Pure traversal helpers + the `RopeWalker` state machine. No GL — headless and unit-testable. |
| `games/02-strandbound/RopeTool.h` (modified) | Exposes the rope list via `ropes()`. |
| `games/02-strandbound/main.cpp` (modified) | Player state, footing query, respawn, `RopeWalker` wiring, lean-meter + win HUD, the traversal-camera view switch. |
| `tests/test_hud.cpp` (modified) | A `setSize` test. |
| `tests/test_rope_walker.cpp` (new) | Helper tests + `RopeWalker` state-machine tests. |
| `tests/CMakeLists.txt` (modified) | Registers `test_rope_walker` (compiles `RopeWalker.cpp` into the test). |
| `docs/engine/strandbound-m5.md` (new) | Concept note. |

---

## Task 1: `Hud::setSize`

The lean-meter fill panel resizes every frame; `Hud` has `setPosition`/`setColor`/`setVisible` but no `setSize`. Add it.

**Files:**
- Modify: `engine/ui/Hud.h`
- Modify: `engine/ui/Hud.cpp`
- Modify: `tests/test_hud.cpp`

- [ ] **Step 1: Write the failing test — add to `tests/test_hud.cpp`**

Add this block inside `main()`, just before `return iron_test_result();`:

```cpp
    // setSize changes a panel's quad extent.
    {
        Hud hud;
        const HudId p = hud.addPanel(Vec2{0, 0}, Vec2{10, 10}, Vec4{1,1,1,1});
        hud.setSize(p, Vec2{40, 20});
        const HudBatch batch = hud.build(font, kWhite);
        const HudDrawGroup* g = groupFor(batch, kWhite);
        CHECK(g != nullptr);
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (const HudVertex& v : g->vertices) {
            if (v.position.x > maxX) maxX = v.position.x;
            if (v.position.y > maxY) maxY = v.position.y;
        }
        CHECK_NEAR(maxX, 40.0f);
        CHECK_NEAR(maxY, 20.0f);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build` then `ctest --test-dir build -C Debug -R test_hud --output-on-failure`
Expected: build FAILS — `Hud` has no member `setSize`.

- [ ] **Step 3: Declare `setSize` in `engine/ui/Hud.h`**

In `class Hud`, directly after the `setColor` declaration, add:

```cpp
    void setSize(HudId id, Vec2 size);
```

- [ ] **Step 4: Implement `setSize` in `engine/ui/Hud.cpp`**

Directly after the `setColor` implementation, add:

```cpp
void Hud::setSize(HudId id, Vec2 size) {
    if (Element* e = get(id)) {
        e->size = size;
    }
}
```

- [ ] **Step 5: Build and run the test**

Run: `cmake --build build` then `ctest --test-dir build -C Debug -R test_hud --output-on-failure`
Expected: `test_hud` passes.

- [ ] **Step 6: Commit**

```bash
git add engine/ui/Hud.h engine/ui/Hud.cpp tests/test_hud.cpp
git commit -m "$(cat <<'EOF'
Add Hud::setSize

Resizes a retained HUD element, alongside setPosition/setColor/
setVisible — needed for the M5 lean meter's fill panel.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `RopeWalker` pure helpers

The pure, headless functions M5's traversal is built from: footing, lean dynamics, the `t` parameter advance, and mount detection. All are unit-tested.

**Files:**
- Create: `games/02-strandbound/RopeWalker.h`
- Create: `games/02-strandbound/RopeWalker.cpp`
- Create: `tests/test_rope_walker.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `games/02-strandbound/RopeWalker.h`**

```cpp
#pragma once

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <vector>

// Strandbound's tightrope traversal. Game-specific gameplay — it lives with
// the game, not the engine. Nothing here touches OpenGL, so it is headless
// and unit-testable.

// --- Pure helpers ---

// True if the world point (x, z) lies within the XZ footprint of any AABB in
// `islands` — i.e. the player has solid ground beneath them.
bool hasFooting(float x, float z, const std::vector<iron::Aabb>& islands);

// The lean-perturbation magnitude (per second). Grows with `timeOnRope`
// (seconds) so a long crossing gets harder; capped so it stays fair.
float leanDriftMagnitude(float timeOnRope);

// The new lean after one step. Lean is unstable — displacement self-amplifies,
// like an inverted pendulum — so the player must actively counter-steer.
// `nudge` is a small random perturbation; `steer` (-1..+1) is the player's
// correction input. Not clamped: the caller tests |lean| >= 1 for a fall.
float applyLean(float lean, float nudge, float steer, float dt);

// Advances a traversal parameter `t` in [0, 1] by `input` (-1..+1) at a walk
// speed of `walkSpeed` units/second along a rope of length `ropeLength`,
// clamped to [0, 1]. A non-positive `ropeLength` leaves `t` unchanged.
float advanceParam(float t, float input, float walkSpeed, float ropeLength,
                   float dt);

// Index of the first rope in `ropes` whose start or end point is within
// `radius` (measured horizontally, in the XZ plane) of `playerFeet`, or -1 if
// none. On a hit, `outAtStart` is set true if the near end is the rope's first
// point, false if it is the last.
int findMountRope(iron::Vec3 playerFeet, const std::vector<iron::Rope>& ropes,
                  float radius, bool& outAtStart);
```

- [ ] **Step 2: Write the failing test — create `tests/test_rope_walker.cpp`**

```cpp
#include "test_framework.h"
#include "RopeWalker.h"
#include "math/Aabb.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <cmath>
#include <vector>

using namespace iron;

int main() {
    // hasFooting: inside an island footprint is true; over the gap is false.
    {
        std::vector<Aabb> islands = {
            Aabb{Vec3{-10, -1, -10}, Vec3{10, 1, 10}},
        };
        CHECK(hasFooting(0.0f, 0.0f, islands));     // centre
        CHECK(hasFooting(9.0f, -9.0f, islands));    // inside a corner
        CHECK(!hasFooting(0.0f, -25.0f, islands));  // over the gap
        CHECK(!hasFooting(15.0f, 0.0f, islands));   // past the x edge
    }

    // leanDriftMagnitude grows with time on the rope.
    {
        CHECK(leanDriftMagnitude(10.0f) > leanDriftMagnitude(0.0f));
    }

    // applyLean: an off-centre lean with no input self-amplifies; a
    // counter-steer opposing the lean shrinks it.
    {
        const float grown = applyLean(0.5f, 0.0f, 0.0f, 0.1f);
        CHECK(grown > 0.5f);
        const float corrected = applyLean(0.5f, 0.0f, -1.0f, 0.1f);
        CHECK(corrected < 0.5f);
        // A pure nudge with no lean and no steer moves lean by the nudge.
        CHECK_NEAR(applyLean(0.0f, 0.05f, 0.0f, 0.1f), 0.05f);
    }

    // advanceParam: clamped to [0,1]; forward/back move t the expected way;
    // a non-positive rope length is a no-op.
    {
        CHECK_NEAR(advanceParam(0.0f, 0.0f, 3.0f, 10.0f, 0.1f), 0.0f);
        CHECK(advanceParam(0.5f, 1.0f, 3.0f, 10.0f, 0.1f) > 0.5f);
        CHECK(advanceParam(0.5f, -1.0f, 3.0f, 10.0f, 0.1f) < 0.5f);
        CHECK_NEAR(advanceParam(1.0f, 1.0f, 3.0f, 10.0f, 1.0f), 1.0f);  // clamp hi
        CHECK_NEAR(advanceParam(0.0f, -1.0f, 3.0f, 10.0f, 1.0f), 0.0f); // clamp lo
        CHECK_NEAR(advanceParam(0.5f, 1.0f, 3.0f, 0.0f, 0.1f), 0.5f);   // no-op
    }

    // findMountRope: a player near a rope's first point gets that rope index
    // with outAtStart true; far from any rope gives -1.
    {
        std::vector<Rope> ropes;
        ropes.push_back(Rope(Vec3{0, 0, 0}, Vec3{10, 0, 0}, 8, 12.0f));
        bool atStart = false;
        const int near = findMountRope(Vec3{0.3f, 0.0f, 0.2f}, ropes, 1.5f,
                                       atStart);
        CHECK(near == 0);
        CHECK(atStart);
        const int nearEnd = findMountRope(Vec3{10.2f, 0.0f, 0.0f}, ropes, 1.5f,
                                          atStart);
        CHECK(nearEnd == 0);
        CHECK(!atStart);
        CHECK(findMountRope(Vec3{50, 0, 50}, ropes, 1.5f, atStart) == -1);
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

`test_rope_walker` must compile the game-side `RopeWalker.cpp` into the test executable (it is not part of the `ironcore` library). Append this to `tests/CMakeLists.txt` (the `iron_add_test` helper takes a single source, so use an explicit block):

```cmake
# test_rope_walker compiles the game-side RopeWalker.cpp directly, since it is
# not part of the ironcore library.
add_executable(test_rope_walker
  test_rope_walker.cpp
  ${CMAKE_SOURCE_DIR}/games/02-strandbound/RopeWalker.cpp)
target_link_libraries(test_rope_walker PRIVATE ironcore)
target_include_directories(test_rope_walker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/02-strandbound)
add_test(NAME test_rope_walker COMMAND test_rope_walker)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build FAILS — `RopeWalker.cpp` does not exist; the helper functions are unresolved.

- [ ] **Step 5: Create `games/02-strandbound/RopeWalker.cpp`**

```cpp
#include "RopeWalker.h"

#include <cstddef>

namespace {
// Lean dynamics.
constexpr float kInstability = 1.6f;  // lean self-amplification, per second
constexpr float kSteerRate = 1.9f;    // counter-steer authority, per second
// leanDriftMagnitude shaping.
constexpr float kDriftBase = 0.15f;    // perturbation/sec at mount
constexpr float kDriftGrowth = 0.05f;  // extra perturbation/sec per second
constexpr float kDriftMax = 0.6f;
}  // namespace

bool hasFooting(float x, float z, const std::vector<iron::Aabb>& islands) {
    for (const iron::Aabb& box : islands) {
        if (x >= box.min.x && x <= box.max.x &&
            z >= box.min.z && z <= box.max.z) {
            return true;
        }
    }
    return false;
}

float leanDriftMagnitude(float timeOnRope) {
    const float m = kDriftBase + kDriftGrowth * timeOnRope;
    return (m < kDriftMax) ? m : kDriftMax;
}

float applyLean(float lean, float nudge, float steer, float dt) {
    return lean + lean * kInstability * dt + nudge + steer * kSteerRate * dt;
}

float advanceParam(float t, float input, float walkSpeed, float ropeLength,
                   float dt) {
    if (ropeLength <= 0.0f) {
        return t;
    }
    float next = t + input * walkSpeed * dt / ropeLength;
    if (next < 0.0f) next = 0.0f;
    if (next > 1.0f) next = 1.0f;
    return next;
}

int findMountRope(iron::Vec3 playerFeet, const std::vector<iron::Rope>& ropes,
                  float radius, bool& outAtStart) {
    const float r2 = radius * radius;
    for (std::size_t i = 0; i < ropes.size(); ++i) {
        const std::vector<iron::VerletPoint>& pts = ropes[i].points();
        if (pts.size() < 2) {
            continue;
        }
        const iron::Vec3 a = pts.front().position;
        const iron::Vec3 b = pts.back().position;
        const float ax = a.x - playerFeet.x;
        const float az = a.z - playerFeet.z;
        if (ax * ax + az * az <= r2) {
            outAtStart = true;
            return static_cast<int>(i);
        }
        const float bx = b.x - playerFeet.x;
        const float bz = b.z - playerFeet.z;
        if (bx * bx + bz * bz <= r2) {
            outAtStart = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}
```

- [ ] **Step 6: Build and run the test**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug -R test_rope_walker --output-on-failure`
Expected: `test_rope_walker` passes.

- [ ] **Step 7: Commit**

```bash
git add games/02-strandbound/RopeWalker.h games/02-strandbound/RopeWalker.cpp tests/test_rope_walker.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add RopeWalker traversal helpers

Pure, headless functions for M5's tightrope traversal: footing query,
lean dynamics (unstable, counter-steered), the t-parameter advance, and
mount detection.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: The `RopeWalker` state machine

The class that owns an active traversal: mount, per-step advance, the lean, the camera (with roll), and the fall / dismount / win outcomes.

**Files:**
- Modify: `games/02-strandbound/RopeWalker.h`
- Modify: `games/02-strandbound/RopeWalker.cpp`
- Modify: `tests/test_rope_walker.cpp`

- [ ] **Step 1: Add the `RopeWalker` class to `games/02-strandbound/RopeWalker.h`**

Add this directly after the `findMountRope` declaration (before the final line of the file), inside the same header:

```cpp
// An active tightrope traversal. Constructed once and reused: begin() starts a
// crossing, step() advances it. Holds no reference to the rope — the rope is
// re-supplied each step.
class RopeWalker {
public:
    // The result of a step (or the state begin() leaves the walker in).
    enum class Result { Traversing, Dismounted, Fell, Won };

    // Start traversing `rope`. `atStart` true means the player mounted at the
    // rope's first point (t=0 there, t=1 at the last point); false means the
    // last point (the t axis is reversed). `yaw`/`pitch` seed the look
    // direction from the player's current facing.
    void begin(const iron::Rope& rope, bool atStart, float yaw, float pitch);

    // Advance one fixed step. `forward` (-1..+1) moves along the rope, `steer`
    // (-1..+1) counter-steers the lean, `mouseDX`/`mouseDY` turn the look,
    // `driftRandom` (-1..+1) is this step's random lean perturbation.
    // `rope` is the rope being traversed; `farIsland` holds the AABB(s) that
    // count as the winning island.
    Result step(float forward, float steer, float mouseDX, float mouseDY,
                float driftRandom, float dt, const iron::Rope& rope,
                const std::vector<iron::Aabb>& farIsland);

    float lean() const { return lean_; }            // -1..1, for the HUD meter
    iron::Mat4 viewMatrix() const;                   // camera, including roll
    iron::Vec3 exitFeet() const { return exitFeet_; }  // where to drop the player

private:
    // The interpolated rope point at the current t_ (no eye-height offset).
    iron::Vec3 sampleRope(const iron::Rope& rope) const;
    // The rope's mounted-end / far-end point, accounting for atStart_.
    iron::Vec3 mountEndPoint(const iron::Rope& rope) const;
    iron::Vec3 farEndPoint(const iron::Rope& rope) const;

    float t_ = 0.0f;
    bool atStart_ = true;
    float lean_ = 0.0f;
    float timeOnRope_ = 0.0f;
    float ropeLength_ = 1.0f;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    iron::Vec3 eye_{};
    iron::Vec3 exitFeet_{};
};
```

- [ ] **Step 2: Write the failing tests — add to `tests/test_rope_walker.cpp`**

Add `#include "math/Mat4.h"` to the includes. Add these blocks inside `main()`, before `return iron_test_result();`:

```cpp
    // A straight rope on the x axis, 8 segments. The far end (10,0,0) sits on
    // a "far island" AABB; the near end (0,0,0) does not.
    auto makeRope = []() {
        return Rope(Vec3{0, 0, 0}, Vec3{10, 0, 0}, 8, 10.5f);
    };
    const std::vector<Aabb> farIsland = {
        Aabb{Vec3{8, -1, -3}, Vec3{13, 1, 3}},
    };
    const float dt = 1.0f / 60.0f;

    // Walking the rope fully forward, with steady counter-steering, reaches
    // the far island and wins.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            // Counter-steer toward zero each step to hold balance.
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Won);
    }

    // No counter-steering and a steady perturbation: the player falls.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            result = walker.step(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Fell);
    }

    // Retreating off the start end dismounts without a win.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        // Step forward a little so t is clearly above 0.
        for (int i = 0; i < 20; ++i) {
            walker.step(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, dt, rope, farIsland);
        }
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(-1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  farIsland);
        }
        CHECK(result == RopeWalker::Result::Dismounted);
    }

    // Crossing fully when the far end is NOT on the win island dismounts,
    // does not win.
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        const std::vector<Aabb> noIsland;  // empty: nothing counts as a win
        RopeWalker::Result result = RopeWalker::Result::Traversing;
        for (int i = 0; i < 5000 && result == RopeWalker::Result::Traversing;
             ++i) {
            const float steer = (walker.lean() > 0.0f) ? -1.0f : 1.0f;
            result = walker.step(1.0f, steer, 0.0f, 0.0f, 0.0f, dt, rope,
                                  noIsland);
        }
        CHECK(result == RopeWalker::Result::Dismounted);
    }

    // With no movement input, the walker stays on the rope (it does not
    // immediately dismount at t=0).
    {
        Rope rope = makeRope();
        RopeWalker walker;
        walker.begin(rope, true, 0.0f, 0.0f);
        const RopeWalker::Result result =
            walker.step(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, dt, rope, farIsland);
        CHECK(result == RopeWalker::Result::Traversing);
    }
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: build FAILS — `RopeWalker` is not defined.

- [ ] **Step 4: Implement `RopeWalker` in `games/02-strandbound/RopeWalker.cpp`**

Add `#include "math/Transform.h"` and `#include <cmath>` to the includes at the top of `RopeWalker.cpp`. Add these constants to the existing anonymous `namespace { ... }` block:

```cpp
// Traversal tuning.
constexpr float kWalkSpeed = 3.0f;      // units/second along the rope
constexpr float kEyeHeight = 1.7f;      // camera height above the rope
constexpr float kMaxRoll = 0.5f;        // camera roll (radians) at |lean| = 1
constexpr float kMouseSensitivity = 0.0025f;
constexpr float kPitchLimit = 1.55334f;  // just under 90 degrees
```

Then add the member function implementations at the end of the file, before the closing of the file (these are free of any namespace — `RopeWalker` is in the global namespace, like `RopeTool`):

```cpp
void RopeWalker::begin(const iron::Rope& rope, bool atStart, float yaw,
                       float pitch) {
    t_ = 0.0f;
    atStart_ = atStart;
    lean_ = 0.0f;
    timeOnRope_ = 0.0f;
    yaw_ = yaw;
    pitch_ = pitch;

    // Rope length is the sum of its segment lengths.
    const std::vector<iron::VerletPoint>& pts = rope.points();
    float len = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        len += iron::length(pts[i].position - pts[i - 1].position);
    }
    ropeLength_ = (len > 1e-4f) ? len : 1.0f;

    eye_ = sampleRope(rope) + iron::Vec3{0.0f, kEyeHeight, 0.0f};
    const iron::Vec3 m = mountEndPoint(rope);
    exitFeet_ = iron::Vec3{m.x, 0.0f, m.z};
}

RopeWalker::Result RopeWalker::step(float forward, float steer, float mouseDX,
                                    float mouseDY, float driftRandom, float dt,
                                    const iron::Rope& rope,
                                    const std::vector<iron::Aabb>& farIsland) {
    timeOnRope_ += dt;

    // Look — same convention as FirstPersonController.
    yaw_ -= mouseDX * kMouseSensitivity;
    pitch_ -= mouseDY * kMouseSensitivity;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;

    // Lean — an unstable balance the player must counter-steer.
    const float nudge = driftRandom * leanDriftMagnitude(timeOnRope_) * dt;
    lean_ = applyLean(lean_, nudge, steer, dt);
    if (lean_ >= 1.0f || lean_ <= -1.0f) {
        return Result::Fell;
    }

    // Move along the rope.
    t_ = advanceParam(t_, forward, kWalkSpeed, ropeLength_, dt);
    eye_ = sampleRope(rope) + iron::Vec3{0.0f, kEyeHeight, 0.0f};

    // Reached the far end.
    if (t_ >= 1.0f) {
        const iron::Vec3 f = farEndPoint(rope);
        exitFeet_ = iron::Vec3{f.x, 0.0f, f.z};
        return hasFooting(f.x, f.z, farIsland) ? Result::Won
                                               : Result::Dismounted;
    }
    // Retreated off the start end (only when actively walking back).
    if (t_ <= 0.0f && forward < 0.0f) {
        const iron::Vec3 m = mountEndPoint(rope);
        exitFeet_ = iron::Vec3{m.x, 0.0f, m.z};
        return Result::Dismounted;
    }
    return Result::Traversing;
}

iron::Vec3 RopeWalker::sampleRope(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    const int n = static_cast<int>(pts.size());
    if (n < 2) {
        return iron::Vec3{};
    }
    // t_ = 0 is the mounted end; map t_ onto the point list.
    const float param =
        atStart_ ? t_ * static_cast<float>(n - 1)
                 : (1.0f - t_) * static_cast<float>(n - 1);
    int i = static_cast<int>(param);
    if (i < 0) i = 0;
    if (i > n - 2) i = n - 2;
    const float frac = param - static_cast<float>(i);
    const iron::Vec3 p0 = pts[i].position;
    const iron::Vec3 p1 = pts[i + 1].position;
    return p0 + (p1 - p0) * frac;
}

iron::Vec3 RopeWalker::mountEndPoint(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    if (pts.empty()) {
        return iron::Vec3{};
    }
    return atStart_ ? pts.front().position : pts.back().position;
}

iron::Vec3 RopeWalker::farEndPoint(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    if (pts.empty()) {
        return iron::Vec3{};
    }
    return atStart_ ? pts.back().position : pts.front().position;
}

iron::Mat4 RopeWalker::viewMatrix() const {
    // Look direction from yaw/pitch (FirstPersonController convention).
    const float cp = std::cos(pitch_);
    const iron::Vec3 forward{
        -std::sin(yaw_) * cp,
        std::sin(pitch_),
        -std::cos(yaw_) * cp,
    };
    // Camera basis, then roll the up vector about the view axis by the lean.
    const iron::Vec3 worldUp{0.0f, 1.0f, 0.0f};
    const iron::Vec3 right = iron::normalize(iron::cross(forward, worldUp));
    const iron::Vec3 up = iron::cross(right, forward);
    const float roll = lean_ * kMaxRoll;
    const iron::Vec3 rolledUp =
        up * std::cos(roll) + right * std::sin(roll);
    return iron::lookAt(eye_, eye_ + forward, rolledUp);
}
```

- [ ] **Step 5: Build and run the tests**

Run: `cmake --build build` then
`ctest --test-dir build -C Debug -R test_rope_walker --output-on-failure`
Expected: `test_rope_walker` passes (all helper + state-machine cases).

- [ ] **Step 6: Commit**

```bash
git add games/02-strandbound/RopeWalker.h games/02-strandbound/RopeWalker.cpp tests/test_rope_walker.cpp
git commit -m "$(cat <<'EOF'
Add the RopeWalker traversal state machine

begin()/step() drive a tightrope crossing: move along the rope, hold an
unstable lean, and resolve to Won / Dismounted / Fell. viewMatrix()
produces the first-person camera with a lean-driven roll.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Strandbound integration

Wire M5 into the game: expose the rope list, add the player state machine, the footing query + respawn, the `RopeWalker` traversal, and the lean-meter + win HUD. Verified by a clean build and by running the game (no new unit test — this is window/GL integration).

**Files:**
- Modify: `games/02-strandbound/RopeTool.h`
- Modify: `games/02-strandbound/main.cpp`

- [ ] **Step 1: Expose the rope list from `RopeTool`**

In `games/02-strandbound/RopeTool.h`, in the public section after `ropeCount()`, add:

```cpp
    // The live ropes — for RopeWalker to read endpoints and points.
    const std::vector<iron::Rope>& ropes() const { return ropes_; }
```

- [ ] **Step 2: Add includes and the random source to `main.cpp`**

In `games/02-strandbound/main.cpp`, add to the include block:

```cpp
#include "RopeWalker.h"
```

and, with the standard-library includes:

```cpp
#include <random>
```

- [ ] **Step 3: Add the player-state enum and constants to `main.cpp`**

In the anonymous `namespace { ... }` block in `main.cpp`, after the existing constants, add:

```cpp
// M5 player state.
enum class PlayerState { Walking, Traversing, Won };

// Where the player (re)spawns: the home-island start.
constexpr iron::Vec3 kStartPos{0.0f, 0.0f, 7.0f};

// How close (horizontally) the player must be to a rope end to mount it.
constexpr float kMountRadius = 1.6f;
```

- [ ] **Step 4: Build the island collider subsets and M5 state in `main()`**

The level's `boxes[]` array has the home island at index 0 and the far island
at index 4 (see its comments). After the loop that fills `colliders`, and
after the `RopeTool ropeTool(...)` line, add:

```cpp
    // Footing: only the two islands provide solid ground. The far island is
    // also the win target.
    const std::vector<iron::Aabb> islandColliders = {colliders[0],
                                                     colliders[4]};
    const std::vector<iron::Aabb> farIsland = {colliders[4]};

    PlayerState state = PlayerState::Walking;
    RopeWalker ropeWalker;
    int traversedRope = -1;

    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> driftDist(-1.0f, 1.0f);
```

- [ ] **Step 5: Add the lean meter and win label to the HUD setup**

In the HUD setup block in `main()` (where the panel, readout, and crosshair
are added), after the crosshair `addImage` call, add:

```cpp
    // Lean meter: a track panel plus a fill panel, bottom-centre. Hidden
    // until the player is traversing a rope.
    constexpr float kMeterW = 240.0f;
    constexpr float kMeterH = 18.0f;
    const float meterX = static_cast<float>(screenW) / 2.0f - kMeterW / 2.0f;
    const float meterY = static_cast<float>(screenH) - 70.0f;
    const iron::HudId meterTrack = hud.addPanel(
        iron::Vec2{meterX, meterY}, iron::Vec2{kMeterW, kMeterH},
        iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    const iron::HudId meterFill = hud.addPanel(
        iron::Vec2{meterX, meterY}, iron::Vec2{0.0f, kMeterH},
        iron::Vec4{0.3f, 0.8f, 0.2f, 0.9f});
    hud.setVisible(meterTrack, false);
    hud.setVisible(meterFill, false);

    // Win label, centred. 21 chars at scale 3 (8px glyphs) is ~504px wide.
    const iron::HudId winLabel = hud.addText(
        "You crossed the gap!",
        iron::Vec2{static_cast<float>(screenW) / 2.0f - 252.0f,
                   static_cast<float>(screenH) / 2.0f - 12.0f},
        3.0f, iron::Vec4{1.0f, 1.0f, 0.4f, 1.0f});
    hud.setVisible(winLabel, false);
```

- [ ] **Step 6: Rewrite the update lambda for the player state machine**

Replace the existing `app.setUpdate([&](const iron::FrameTime& time) { ... });`
block with this. It keeps the Escape-to-quit and the existing controller /
rope-tool logic for the `Walking` state, and adds `Traversing`:

```cpp
    app.setUpdate([&](const iron::FrameTime& time) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
        const float dt = time.deltaSeconds;

        if (state == PlayerState::Walking) {
            iron::ControllerInput ci;
            if (input.keyDown(GLFW_KEY_W)) ci.forward += 1.0f;
            if (input.keyDown(GLFW_KEY_S)) ci.forward -= 1.0f;
            if (input.keyDown(GLFW_KEY_D)) ci.strafe += 1.0f;
            if (input.keyDown(GLFW_KEY_A)) ci.strafe -= 1.0f;
            ci.mouseDX = static_cast<float>(input.mouseDeltaX());
            ci.mouseDY = static_cast<float>(input.mouseDeltaY());
            player.update(ci, dt);

            const bool place =
                input.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
            const bool tie = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
            const bool cut = input.keyPressed(GLFW_KEY_C);
            ropeTool.update(player.aimRay(), player.position(), place, tie,
                            cut, dt);

            // Footing: stepping off solid ground respawns the player.
            if (!hasFooting(player.position().x, player.position().z,
                            islandColliders)) {
                player.setPosition(kStartPos);
            }

            // Mounting: stepping onto a rope end starts a traversal.
            bool atStart = true;
            const int rope = findMountRope(player.position(),
                                           ropeTool.ropes(), kMountRadius,
                                           atStart);
            if (rope >= 0) {
                traversedRope = rope;
                ropeWalker.begin(ropeTool.ropes()[static_cast<std::size_t>(
                                     rope)],
                                 atStart, player.yaw(), player.pitch());
                state = PlayerState::Traversing;
            }
        } else if (state == PlayerState::Traversing) {
            float forward = 0.0f;
            if (input.keyDown(GLFW_KEY_W)) forward += 1.0f;
            if (input.keyDown(GLFW_KEY_S)) forward -= 1.0f;
            float steer = 0.0f;
            if (input.keyDown(GLFW_KEY_D)) steer += 1.0f;
            if (input.keyDown(GLFW_KEY_A)) steer -= 1.0f;
            const float mdx = static_cast<float>(input.mouseDeltaX());
            const float mdy = static_cast<float>(input.mouseDeltaY());

            const RopeWalker::Result result = ropeWalker.step(
                forward, steer, mdx, mdy, driftDist(rng), dt,
                ropeTool.ropes()[static_cast<std::size_t>(traversedRope)],
                farIsland);

            if (result == RopeWalker::Result::Won) {
                player.setPosition(ropeWalker.exitFeet());
                state = PlayerState::Won;
            } else if (result == RopeWalker::Result::Fell) {
                player.setPosition(kStartPos);
                state = PlayerState::Walking;
            } else if (result == RopeWalker::Result::Dismounted) {
                player.setPosition(ropeWalker.exitFeet());
                state = PlayerState::Walking;
            }
        }
        // PlayerState::Won — terminal; only Escape (handled above) responds.

        // HUD: the lean meter tracks the traversal; the win label shows on Won.
        const bool traversing = (state == PlayerState::Traversing);
        hud.setVisible(meterTrack, traversing);
        hud.setVisible(meterFill, traversing);
        if (traversing) {
            const float mag = std::fabs(ropeWalker.lean());  // 0..1
            hud.setSize(meterFill,
                        iron::Vec2{kMeterW * mag, kMeterH});
            hud.setColor(meterFill,
                         iron::Vec4{0.3f + 0.6f * mag, 0.8f - 0.6f * mag,
                                    0.2f, 0.9f});
        }
        hud.setVisible(winLabel, state == PlayerState::Won);
    });
```

Add `#include <cmath>` and `#include <cstddef>` to `main.cpp`'s includes if not already present (for `std::fabs` and `std::size_t`).

- [ ] **Step 7: Switch the camera in the render lambda**

In the `app.setRender([&] { ... })` lambda, the view matrix is currently
`const iron::Mat4 view = player.viewMatrix();`. Replace that line with:

```cpp
        const iron::Mat4 view = (state == PlayerState::Traversing)
                                    ? ropeWalker.viewMatrix()
                                    : player.viewMatrix();
```

Leave the rest of the render lambda unchanged — the scene, `ropeTool.draw`,
`flushDebugLines`, and the HUD draw all already use `view`.

- [ ] **Step 8: Build and run the test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: clean build; all tests pass (11 total — the 10 prior plus
`test_rope_walker`).

- [ ] **Step 9: Commit**

```bash
git add games/02-strandbound/RopeTool.h games/02-strandbound/main.cpp
git commit -m "$(cat <<'EOF'
Strandbound: real gap, rope traversal, and a win

main.cpp gains a Walking/Traversing/Won player state: a footing query
respawns the player off solid ground, RopeWalker drives tightrope
crossings with a lean-meter HUD, and reaching the far island wins.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Concept note

**Files:**
- Create: `docs/engine/strandbound-m5.md`

- [ ] **Step 1: Create `docs/engine/strandbound-m5.md`**

```markdown
# Strandbound M5 — Bridge the Gap

M5 is the final milestone of the Strandbound mechanic demo. It turns the
sandbox (place / tie / cut ropes) into a game with a goal.

## The real gap

The `FirstPersonController` clamps the player to flat ground at y=0. The game
makes the gap real with a per-frame **footing query** (`hasFooting`): the
player's XZ position is tested against the home- and far-island AABBs. Off
solid ground, the player is respawned at the home-island start — the placed
anchors and tied ropes are kept.

## Tightrope traversal

`RopeWalker` (game-side, headless, unit-tested) owns a crossing:

- **Mount** — walking within `kMountRadius` of a rope's end anchor starts a
  traversal (`findMountRope` + `RopeWalker::begin`).
- **Move** — a parameter `t` in [0,1] runs along the rope; the camera samples
  the rope's (sagging) curve.
- **Balance** — a signed `lean` is unstable (it self-amplifies); the player
  counter-steers with A/D. The camera rolls with the lean and a HUD meter
  shows how close a fall is.
- **Outcome** — `|lean|` reaching 1 is a fall (respawn); reaching the far end
  on the far island wins; retreating to the start dismounts.

## Player state

The game holds a `Walking` / `Traversing` / `Won` state. While `Traversing`
the `FirstPersonController` is suspended and `RopeWalker` produces the view
matrix (including the lean roll, built with the engine's `lookAt` and a tilted
up vector). On `Won`, a HUD label shows and the demo is complete.

## Engine change

The only engine addition is `Hud::setSize`, used to resize the lean meter's
fill panel each frame. Everything else is game-side, built on existing engine
primitives.
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/strandbound-m5.md
git commit -m "$(cat <<'EOF'
Add Strandbound M5 concept note

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Done

After Task 5, M5 is complete: the gap is real (footing + respawn), ropes are
tightrope-walkable (`RopeWalker` — mount, lean balance, camera roll), and
reaching the far island wins. The playable Strandbound mechanic demo is
finished. Hand off to `superpowers:finishing-a-development-branch`.
