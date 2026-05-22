# Strandbound M6 "Rope Throwing" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace instant infinite-range anchor placement with a charged rope throw — the rope's far end flies in an arc and sticks where it lands — and cap deployed ropes with a finite pool.

**Architecture:** A new headless, unit-tested game-side `RopeThrower` (a charge → throw → projectile → land/fail state machine). `RopeTool` is reworked: it drops placement / tying / free anchors and gains a rope pool. `main.cpp` wires the charge-throw input and the HUD. Entirely game-side — no engine changes.

**Tech Stack:** C++23, OpenGL 3.3 via the engine RHI, CMake, MSVC, the project's custom CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-22-strandbound-m6-rope-throwing-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `games/02-strandbound/RopeThrower.h/.cpp` (new) | The charge → throw → projectile → land/fail state machine. Headless, no GL, unit-tested. |
| `games/02-strandbound/RopeTool.h/.cpp` (modified) | Loses placement / tying / free anchors / the aim marker; keeps the rope collection, drawing, cut; gains a finite rope pool. |
| `games/02-strandbound/main.cpp` (modified) | Charge-throw input, `addRope` on a landed throw, the HUD charge bar + rope readout, the in-flight projectile marker. |
| `games/02-strandbound/CMakeLists.txt` (modified) | Registers `RopeThrower.cpp`. |
| `tests/test_rope_thrower.cpp` (new) | Charge ramp, charge→speed, the projectile arc, collision, miss, throw-chaining gate. |
| `tests/CMakeLists.txt` (modified) | Registers `test_rope_thrower`. |
| `docs/engine/strandbound-m6.md` (new) | Concept note. |

**Task ordering note:** Task 1 (`RopeThrower`) is purely additive — the build stays green. Task 2 reworks `RopeTool`'s interface *and* its only caller (`main.cpp`) together in one task, because a half-done rework would not compile.

---

## Task 1: `RopeThrower`

The headless throw state machine: **Idle → Charging → InFlight → (Idle)**, emitting a per-step event.

**Files:**
- Create: `games/02-strandbound/RopeThrower.h`
- Create: `games/02-strandbound/RopeThrower.cpp`
- Create: `tests/test_rope_thrower.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `games/02-strandbound/RopeThrower.h`**

```cpp
#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

#include <vector>

// Strandbound's rope-throwing mechanic. The player charges a throw and
// releases; the rope's far end flies as a projectile under gravity and sticks
// where it lands. Game-specific gameplay — it lives with the game, not the
// engine. Nothing here touches OpenGL, so it is headless and unit-testable.
class RopeThrower {
public:
    enum class State { Idle, Charging, InFlight };

    // What happened on a given update step.
    enum class Event { None, Landed, Missed };

    // Advance one fixed step.
    //   throwHeld  - is the throw button down this step
    //   hasRope    - is a rope available to throw (the pool is non-empty)
    //   eye        - the player's eye position (the projectile launch point)
    //   lookDir    - the player's look direction (unit length)
    //   feet       - the player's feet position (the rope's near end on a throw)
    //   colliders  - the world boxes the projectile can stick to
    // Returns Landed (a rope should be created from ropeNearEnd to ropeFarEnd),
    // Missed (the throw failed), or None.
    Event update(bool throwHeld, bool hasRope, iron::Vec3 eye,
                 iron::Vec3 lookDir, iron::Vec3 feet,
                 const std::vector<iron::Aabb>& colliders, float dt);

    State state() const { return state_; }
    float charge() const { return charge_; }  // 0..1, for the HUD bar
    iron::Vec3 projectilePosition() const { return projectilePos_; }

    // Valid after an update that returned Event::Landed.
    iron::Vec3 ropeNearEnd() const { return nearEnd_; }
    iron::Vec3 ropeFarEnd() const { return farEnd_; }

private:
    State state_ = State::Idle;
    float charge_ = 0.0f;
    bool armed_ = true;  // false from launch until the button is released

    iron::Vec3 projectilePos_{};
    iron::Vec3 projectileVel_{};
    iron::Vec3 nearEnd_{};
    iron::Vec3 farEnd_{};
};
```

- [ ] **Step 2: Write the failing test — create `tests/test_rope_thrower.cpp`**

```cpp
#include "test_framework.h"
#include "RopeThrower.h"
#include "math/Aabb.h"
#include "math/Vec.h"

#include <vector>

using namespace iron;

int main() {
    const float dt = 1.0f / 60.0f;
    const std::vector<Aabb> noBoxes;

    // Holding the button moves Idle -> Charging and ramps charge toward 1.
    {
        RopeThrower t;
        t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::Charging);
        const float c1 = t.charge();
        for (int i = 0; i < 10; ++i) {
            t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        CHECK(t.charge() > c1);
        for (int i = 0; i < 1000; ++i) {
            t.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        CHECK_NEAR(t.charge(), 1.0f);
    }

    // With no rope available, charging never begins.
    {
        RopeThrower t;
        t.update(true, false, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::Idle);
    }

    // Releasing launches: the thrower goes InFlight and the projectile leaves
    // the eye and travels forward.
    {
        RopeThrower t;
        for (int i = 0; i < 5; ++i) {
            t.update(true, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                     noBoxes, dt);
        }
        t.update(false, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                 noBoxes, dt);
        CHECK(t.state() == RopeThrower::State::InFlight);
        const Vec3 p0 = t.projectilePosition();
        t.update(false, true, Vec3{0,1,0}, Vec3{1,0,0}, Vec3{0,0,0},
                 noBoxes, dt);
        CHECK(t.projectilePosition().x > p0.x);
    }

    // A fuller charge throws faster — the projectile travels further per step.
    {
        RopeThrower brief;
        brief.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        brief.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const Vec3 b0 = brief.projectilePosition();
        brief.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const float briefStep = brief.projectilePosition().x - b0.x;

        RopeThrower full;
        for (int i = 0; i < 200; ++i) {
            full.update(true, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        full.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const Vec3 f0 = full.projectilePosition();
        full.update(false, true, Vec3{}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        const float fullStep = full.projectilePosition().x - f0.x;

        CHECK(fullStep > briefStep);
    }

    // Arc: thrown level, x advances monotonically and height only falls.
    {
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            t.update(true, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        t.update(false, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        Vec3 prev = t.projectilePosition();
        for (int i = 0; i < 30; ++i) {
            t.update(false, true, Vec3{0,5,0}, Vec3{1,0,0}, Vec3{}, noBoxes,
                     dt);
            const Vec3 cur = t.projectilePosition();
            CHECK(cur.x > prev.x);
            CHECK(cur.y <= prev.y + 1e-4f);
            prev = cur;
        }
    }

    // Collision: a throw into a box lands on it; the near end is the feet
    // passed at release.
    {
        const std::vector<Aabb> boxes = {
            Aabb{Vec3{5, -5, -5}, Vec3{15, 5, 5}},
        };
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                     boxes, dt);
        }
        t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                 boxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,0,1},
                          boxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Landed);
        CHECK(t.ropeFarEnd().x > 4.5f && t.ropeFarEnd().x < 5.5f);
        CHECK_NEAR(t.ropeNearEnd().z, 1.0f);
    }

    // Miss: a throw with no boxes falls into the void and reports Missed.
    {
        RopeThrower t;
        for (int i = 0; i < 30; ++i) {
            t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        }
        t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, noBoxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{},
                          noBoxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Missed);
    }

    // A held button does not chain throws: after a throw resolves with the
    // button still down, charging only resumes once the button is released.
    {
        const std::vector<Aabb> boxes = {
            Aabb{Vec3{5, -5, -5}, Vec3{15, 5, 5}},
        };
        RopeThrower t;
        for (int i = 0; i < 200; ++i) {
            t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        }
        t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        RopeThrower::Event ev = RopeThrower::Event::None;
        for (int i = 0; i < 5000 && ev == RopeThrower::Event::None; ++i) {
            ev = t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{},
                          boxes, dt);
        }
        CHECK(ev == RopeThrower::Event::Landed);
        t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        CHECK(t.state() == RopeThrower::State::Idle);  // still held: no recharge
        t.update(false, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        t.update(true, true, Vec3{0,0,0}, Vec3{1,0,0}, Vec3{}, boxes, dt);
        CHECK(t.state() == RopeThrower::State::Charging);  // released, now ok
    }

    return iron_test_result();
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

`test_rope_thrower` compiles the game-side `RopeThrower.cpp` directly (it is not part of the `ironcore` library). Append to `tests/CMakeLists.txt` — use an explicit block, like the existing `test_rope_walker` entry:

```cmake
# test_rope_thrower compiles the game-side RopeThrower.cpp directly, since it
# is not part of the ironcore library.
add_executable(test_rope_thrower
  test_rope_thrower.cpp
  ${CMAKE_SOURCE_DIR}/games/02-strandbound/RopeThrower.cpp)
target_link_libraries(test_rope_thrower PRIVATE ironcore)
target_include_directories(test_rope_thrower PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/games/02-strandbound)
add_test(NAME test_rope_thrower COMMAND test_rope_thrower)
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `cmake -S . -B build` then `cmake --build build`
Expected: build FAILS — `RopeThrower.cpp` does not exist; `RopeThrower` members are unresolved.

- [ ] **Step 5: Create `games/02-strandbound/RopeThrower.cpp`**

```cpp
#include "RopeThrower.h"

#include "math/Ray.h"

namespace {
constexpr float kChargeTime = 1.0f;      // seconds to reach full charge
constexpr float kMinThrowSpeed = 9.0f;   // units/sec at zero charge
constexpr float kMaxThrowSpeed = 28.0f;  // units/sec at full charge
constexpr float kGravity = -20.0f;       // projectile gravity, units/sec^2
constexpr float kKillY = -25.0f;         // below this, the throw has failed
}  // namespace

RopeThrower::Event RopeThrower::update(bool throwHeld, bool hasRope,
                                       iron::Vec3 eye, iron::Vec3 lookDir,
                                       iron::Vec3 feet,
                                       const std::vector<iron::Aabb>& colliders,
                                       float dt) {
    // Re-arm once the button is released, so holding it does not chain throws.
    if (!throwHeld) {
        armed_ = true;
    }

    switch (state_) {
        case State::Idle:
            if (throwHeld && hasRope && armed_) {
                state_ = State::Charging;
                charge_ = 0.0f;
            }
            return Event::None;

        case State::Charging:
            if (throwHeld) {
                charge_ += dt / kChargeTime;
                if (charge_ > 1.0f) {
                    charge_ = 1.0f;
                }
                return Event::None;
            }
            // Button released — launch.
            armed_ = false;
            nearEnd_ = feet;
            projectilePos_ = eye;
            projectileVel_ =
                lookDir * (kMinThrowSpeed +
                           (kMaxThrowSpeed - kMinThrowSpeed) * charge_);
            state_ = State::InFlight;
            return Event::None;

        case State::InFlight: {
            projectileVel_.y += kGravity * dt;
            const iron::Vec3 next = projectilePos_ + projectileVel_ * dt;

            // Test the step's travel segment against the world boxes.
            const iron::Vec3 delta = next - projectilePos_;
            const float segLen = iron::length(delta);
            if (segLen > 1e-6f) {
                const iron::Ray ray{projectilePos_, delta * (1.0f / segLen)};
                float bestT = 1e30f;
                bool hit = false;
                for (const iron::Aabb& box : colliders) {
                    float t = 0.0f;
                    if (iron::intersectRayAabb(ray, box, t) && t <= segLen &&
                        t < bestT) {
                        bestT = t;
                        hit = true;
                    }
                }
                if (hit) {
                    farEnd_ = projectilePos_ + ray.direction * bestT;
                    state_ = State::Idle;
                    charge_ = 0.0f;
                    return Event::Landed;
                }
            }

            projectilePos_ = next;
            if (projectilePos_.y < kKillY) {
                state_ = State::Idle;
                charge_ = 0.0f;
                return Event::Missed;
            }
            return Event::None;
        }
    }
    return Event::None;
}
```

- [ ] **Step 6: Build and run the test**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug -R test_rope_thrower --output-on-failure`
Expected: `test_rope_thrower` passes.

- [ ] **Step 7: Commit**

```bash
git add games/02-strandbound/RopeThrower.h games/02-strandbound/RopeThrower.cpp tests/test_rope_thrower.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add RopeThrower — the charged-throw state machine

A headless Idle/Charging/InFlight state machine: hold to charge,
release to launch the rope's far end as a projectile under gravity; it
reports Landed (with the rope's endpoints) or Missed.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Convert the game to rope-throwing

Rework `RopeTool` (drop placement / tying / free anchors / the aim marker; add the rope pool) and rewrite the `main.cpp` rope interaction (charge-throw input, `addRope` on a landed throw, the HUD charge bar + rope readout, the projectile marker). `RopeTool`'s interface and its only caller change together so the build stays green. No new unit test — `RopeTool` is GL-side; verified by a clean build, the existing 12 tests passing, and running the game.

**Files:**
- Modify: `games/02-strandbound/RopeTool.h`
- Modify: `games/02-strandbound/RopeTool.cpp`
- Modify: `games/02-strandbound/main.cpp`
- Modify: `games/02-strandbound/CMakeLists.txt`

- [ ] **Step 1: Replace `games/02-strandbound/RopeTool.h` entirely**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"
#include "render/Renderer.h"

#include <vector>

// The Strandbound rope collection: it owns the tied ropes, draws them, holds a
// finite pool of ropes the player can still deploy, and handles cutting. Ropes
// are created by RopeThrower (a thrown rope that landed) via addRope.
// Game-specific — it lives with the game, not the engine.
class RopeTool {
public:
    RopeTool(iron::Renderer& renderer, iron::ShaderHandle litShader);

    // Adds a rope between two world points if the pool is non-empty: builds a
    // slack Rope, spends one from the pool, and returns true. Returns false
    // and adds nothing when the pool is empty.
    bool addRope(iron::Vec3 nearEnd, iron::Vec3 farEnd);

    // Advance one fixed step: if `cutPressed`, cut the rope under `aim` (which
    // refunds it to the pool); then step every rope's physics.
    void update(const iron::Ray& aim, bool cutPressed, float dt);

    // Rebuild and draw the rope tube mesh, and queue an endpoint marker at
    // each rope end as debug lines. Call between submitting the scene and
    // flushDebugLines.
    void draw(iron::Renderer& renderer, const iron::Mat4& view,
              const iron::Mat4& projection) const;

    // Ropes the player can still deploy — for the HUD readout.
    int ropesAvailable() const { return ropesAvailable_; }

    // The live ropes — for RopeWalker to read endpoints and points.
    const std::vector<iron::Rope>& ropes() const { return ropes_; }

private:
    // Index of the rope whose points the aim ray passes nearest, or -1.
    int pickRope(const iron::Ray& aim) const;

    std::vector<iron::Rope> ropes_;
    int ropesAvailable_;

    iron::ShaderHandle litShader_ = iron::kInvalidHandle;
    iron::TextureHandle ropeTexture_ = iron::kInvalidHandle;
    iron::MeshHandle ropesMesh_ = iron::kInvalidHandle;
};
```

- [ ] **Step 2: Replace `games/02-strandbound/RopeTool.cpp` entirely**

```cpp
#include "RopeTool.h"

#include "core/Platform.h"
#include "physics/VerletPoint.h"
#include "scene/Mesh.h"

#include <cstddef>
#include <vector>

namespace {
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;     // rope length vs. endpoint span
constexpr float kRopePickRadius = 0.3f;   // rope points picked as spheres
constexpr float kMarkerSize = 0.25f;      // endpoint marker cross half-length
constexpr float kRopeRadius = 0.055f;     // visual rope thickness
constexpr int kRopeSides = 6;             // low-poly tube cross-section
constexpr int kStartingRopes = 5;         // ropes the player starts with
}  // namespace

RopeTool::RopeTool(iron::Renderer& renderer, iron::ShaderHandle litShader)
    : ropesAvailable_(kStartingRopes), litShader_(litShader) {
    // The rope texture ships next to the executable. The rope mesh is created
    // empty and refreshed every frame in draw().
    ropeTexture_ =
        renderer.loadTexture(iron::executableDir() + "/assets/rope.jpg");
    ropesMesh_ = renderer.createMesh(iron::MeshData{});
}

bool RopeTool::addRope(iron::Vec3 nearEnd, iron::Vec3 farEnd) {
    if (ropesAvailable_ <= 0) {
        return false;
    }
    const float span = iron::length(farEnd - nearEnd);
    ropes_.push_back(
        iron::Rope(nearEnd, farEnd, kRopeSegments, span * kSlackFactor));
    --ropesAvailable_;
    return true;
}

int RopeTool::pickRope(const iron::Ray& aim) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < ropes_.size(); ++i) {
        for (const iron::VerletPoint& p : ropes_[i].points()) {
            float t = 0.0f;
            if (iron::intersectRaySphere(aim, p.position, kRopePickRadius, t)
                    && t < bestT) {
                bestT = t;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

void RopeTool::update(const iron::Ray& aim, bool cutPressed, float dt) {
    if (cutPressed) {
        const int rope = pickRope(aim);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin() +
                         static_cast<std::ptrdiff_t>(rope));
            ++ropesAvailable_;  // a cut rope is recovered to the pool
        }
    }
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }
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

    iron::DrawCall ropeCall;
    ropeCall.mesh = ropesMesh_;
    ropeCall.shader = litShader_;
    ropeCall.texture = ropeTexture_;
    renderer.submit(ropeCall, view, projection);

    // A small yellow cross at each rope's two endpoints — the mount points.
    const iron::Vec3 markerColor{0.95f, 0.8f, 0.2f};
    const float s = kMarkerSize;
    for (const iron::Rope& r : ropes_) {
        if (r.points().size() < 2) {
            continue;
        }
        const iron::Vec3 ends[2] = {r.points().front().position,
                                    r.points().back().position};
        for (const iron::Vec3& e : ends) {
            renderer.drawLine(e - iron::Vec3{s, 0.0f, 0.0f},
                              e + iron::Vec3{s, 0.0f, 0.0f}, markerColor);
            renderer.drawLine(e - iron::Vec3{0.0f, s, 0.0f},
                              e + iron::Vec3{0.0f, s, 0.0f}, markerColor);
            renderer.drawLine(e - iron::Vec3{0.0f, 0.0f, s},
                              e + iron::Vec3{0.0f, 0.0f, s}, markerColor);
        }
    }
}
```

- [ ] **Step 3: `main.cpp` — add the `RopeThrower` include and update the title**

In `games/02-strandbound/main.cpp`, add to the includes (next to `#include "RopeTool.h"` / `#include "RopeWalker.h"`):

```cpp
#include "RopeThrower.h"
```

Change the window title line from `Strandbound (M5)` to `Strandbound (M6)`:

```cpp
    config.title = "Iron Core Engine - Strandbound (M6)";
```

- [ ] **Step 4: `main.cpp` — construct `RopeTool` and `RopeThrower`**

The `RopeTool` constructor no longer takes the collider list. Change:

```cpp
    RopeTool ropeTool(colliders, renderer, shader);
```

to:

```cpp
    RopeTool ropeTool(renderer, shader);
    RopeThrower ropeThrower;
```

(The `colliders` vector is still built and is still used for footing and is now passed to `ropeThrower.update`.)

- [ ] **Step 5: `main.cpp` — update the HUD readout to show the rope count**

Replace the readout panel + text block:

```cpp
    // A dark backing panel behind the readout: wide enough for the longest
    // readout string at scale 2.0, with 8px padding around the 16px text.
    hud.addPanel(iron::Vec2{8.0f, 8.0f}, iron::Vec2{408.0f, 32.0f},
                 iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    // The status readout (text); its id is kept so it can be updated.
    const iron::HudId readout = hud.addText(
        "Anchors: 0   Ropes: 0", iron::Vec2{16.0f, 16.0f}, 2.0f,
        iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
```

with:

```cpp
    // A dark backing panel behind the rope-count readout.
    hud.addPanel(iron::Vec2{8.0f, 8.0f}, iron::Vec2{160.0f, 32.0f},
                 iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    // The rope-count readout (text); its id is kept so it can be updated.
    const iron::HudId readout = hud.addText(
        "Ropes: 5", iron::Vec2{16.0f, 16.0f}, 2.0f,
        iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
```

- [ ] **Step 6: `main.cpp` — add the charge-bar HUD elements**

Directly after the lean-meter HUD block (after the `hud.setVisible(meterFill, false);` line) and before the win-label block, add:

```cpp
    // Charge bar: a track panel plus a fill panel, bottom-centre, shown only
    // while a throw is charging.
    constexpr float kChargeW = 240.0f;
    constexpr float kChargeH = 18.0f;
    const float chargeX = static_cast<float>(screenW) / 2.0f - kChargeW / 2.0f;
    const float chargeY = static_cast<float>(screenH) - 104.0f;
    const iron::HudId chargeTrack = hud.addPanel(
        iron::Vec2{chargeX, chargeY}, iron::Vec2{kChargeW, kChargeH},
        iron::Vec4{0.0f, 0.0f, 0.0f, 0.55f});
    const iron::HudId chargeFill = hud.addPanel(
        iron::Vec2{chargeX, chargeY}, iron::Vec2{0.0f, kChargeH},
        iron::Vec4{0.95f, 0.75f, 0.2f, 0.9f});
    hud.setVisible(chargeTrack, false);
    hud.setVisible(chargeFill, false);
```

- [ ] **Step 7: `main.cpp` — replace the place/tie/cut input with charge-throw**

In the update lambda's `Walking` branch, replace this block:

```cpp
            const bool place =
                input.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
            const bool tie = input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
            const bool cut = input.keyPressed(GLFW_KEY_C);
            // ropeTool.update is called only here (Walking) — ropes_ is never
            // mutated while Traversing, so traversedRope stays valid.
            ropeTool.update(player.aimRay(), player.position(), place, tie,
                            cut, dt);
```

with:

```cpp
            // Rope throwing: hold left-click to charge, release to throw.
            const iron::Ray aim = player.aimRay();
            const RopeThrower::Event throwEvent = ropeThrower.update(
                input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT),
                ropeTool.ropesAvailable() > 0, aim.origin, aim.direction,
                player.position(), colliders, dt);
            if (throwEvent == RopeThrower::Event::Landed) {
                ropeTool.addRope(ropeThrower.ropeNearEnd(),
                                 ropeThrower.ropeFarEnd());
            }
            // ropeTool.update is called only here (Walking) — ropes_ is never
            // mutated while Traversing, so traversedRope stays valid.
            ropeTool.update(aim, input.keyPressed(GLFW_KEY_C), dt);
```

- [ ] **Step 8: `main.cpp` — remove the now-deleted `clearAimTarget` call**

In the `Walking` branch's mount block, remove the line `ropeTool.clearAimTarget();` (RopeTool no longer has that method). The block becomes:

```cpp
                if (rope >= 0) {
                    traversedRope = rope;
                    ropeWalker.begin(
                        ropeTool.ropes()[static_cast<std::size_t>(rope)],
                        atStart, player.yaw(), player.pitch());
                    state = PlayerState::Traversing;
                }
```

- [ ] **Step 9: `main.cpp` — drive the charge bar each frame**

In the update lambda, after the line `hud.setVisible(winLabel, state == PlayerState::Won);`, add:

```cpp
        // Charge bar: visible only while a throw is charging; fill tracks
        // the charge level.
        const bool charging =
            (ropeThrower.state() == RopeThrower::State::Charging);
        hud.setVisible(chargeTrack, charging);
        hud.setVisible(chargeFill, charging);
        if (charging) {
            hud.setSize(chargeFill, iron::Vec2{kChargeW * ropeThrower.charge(),
                                               kChargeH});
        }
```

- [ ] **Step 10: `main.cpp` — update the readout text and draw the projectile marker**

In the render lambda, replace the readout-refresh line:

```cpp
        hud.setText(readout,
                    "Anchors: " + std::to_string(ropeTool.anchorCount()) +
                        "   Ropes: " + std::to_string(ropeTool.ropeCount()));
```

with:

```cpp
        hud.setText(readout,
                    "Ropes: " + std::to_string(ropeTool.ropesAvailable()));
```

And in the render lambda, between the `ropeTool.draw(...)` call and the
`renderer.flushDebugLines(...)` call, add the in-flight projectile marker:

```cpp
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
```

- [ ] **Step 11: Register `RopeThrower.cpp` in `games/02-strandbound/CMakeLists.txt`**

Add `RopeThrower.cpp` to the game's `add_executable` source list, alongside `main.cpp`, `RopeTool.cpp`, and `RopeWalker.cpp`. For example, if the line reads
`add_executable(strandbound main.cpp RopeTool.cpp RopeWalker.cpp)`, change it to
`add_executable(strandbound main.cpp RopeTool.cpp RopeWalker.cpp RopeThrower.cpp)`.
(Read the file first — match its exact existing formatting.)

- [ ] **Step 12: Build and run the full test suite**

Run: `cmake -S . -B build` then `cmake --build build` then
`ctest --test-dir build -C Debug --output-on-failure`
Expected: clean build; all 12 tests pass (the 11 prior plus `test_rope_thrower`).

- [ ] **Step 13: Commit**

```bash
git add games/02-strandbound/RopeTool.h games/02-strandbound/RopeTool.cpp games/02-strandbound/main.cpp games/02-strandbound/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Strandbound: replace anchor placement with rope throwing

RopeTool drops placement, tying, and free anchors and gains a finite
rope pool; main.cpp wires the charged throw (hold to charge, release to
fling the rope's far end), a HUD charge bar and rope-count readout, and
the in-flight projectile marker.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Concept note

**Files:**
- Create: `docs/engine/strandbound-m6.md`

- [ ] **Step 1: Create `docs/engine/strandbound-m6.md`**

```markdown
# Strandbound M6 — Rope Throwing

M6 makes bridging the gap a skill. Earlier milestones placed an anchor with an
instant, infinite-range raycast — so crossing was trivial. M6 replaces
placement and tying with a single verb: throw the rope.

## RopeThrower

`RopeThrower` (game-side, headless, unit-tested) is a small state machine —
`Idle → Charging → InFlight`:

- **Charging** — holding the throw button ramps a `charge` value 0→1 over a
  fixed time; a HUD bar shows it.
- **Launch** — releasing flings the rope's far end from the player's eye, in
  the look direction, at a speed mapped from `charge`.
- **InFlight** — the far end is a projectile under gravity. Each step its
  travel segment is tested against the world boxes (`intersectRayAabb`).
- **Outcome** — a hit reports `Landed` with the rope's endpoints (near = the
  player's feet at release, far = the impact point); falling into the void
  reports `Missed`.

A held button cannot chain throws — it must be released between throws.

## The rope pool

`RopeTool` holds a finite pool of ropes. A landed throw spends one
(`addRope`); cutting a deployed rope (`C`) refunds one. A missed throw costs
nothing. The HUD shows the remaining count. There is no soft-lock.

`RopeTool` otherwise just owns the rope collection, draws the tube meshes and
endpoint markers, and steps the rope physics — placement, tying, and free
anchors are gone.

## Unchanged

Once a rope exists it is an ordinary `Rope`: mounting it and tightrope-walking
it (M5's `RopeWalker`), footing, respawn, and the win are all unchanged. M6 is
entirely game-side — no engine change.
```

- [ ] **Step 2: Commit**

```bash
git add docs/engine/strandbound-m6.md
git commit -m "$(cat <<'EOF'
Add Strandbound M6 concept note

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Done

After Task 3, M6 is complete: bridging the gap is a charged-throw skill, a
finite rope pool caps deployed ropes, and the HUD shows the charge and the
count. The M5 traversal, footing, and win are unchanged. Hand off to
`superpowers:finishing-a-development-branch`.
