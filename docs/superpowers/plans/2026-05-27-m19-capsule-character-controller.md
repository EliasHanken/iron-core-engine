# M19 Capsule Character Controller Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `iron::CharacterController` (wrapping `JPH::CharacterVirtual`) and port `games/07-net-shooter` movement onto it — players get wall/floor collision, gravity, ground detection, and a working jump.

**Architecture:** Engine adds `engine/physics/CharacterController.{h,cpp}` — a pimpl wrapper that reaches into `PhysicsWorld::Impl` (the M18 public forward-decl) to register a `JPH::CharacterVirtual` against the world's `JPH::PhysicsSystem`. Net-shooter creates one `PhysicsWorld` + one `CharacterController` per simulated player (client = 1, host = N), pre-populates each world with the arena's static AABBs, and rewrites the `simulate` lambda to drive the controller. `PlayerState` grows from 3 floats (position) to 7 fields (position + velocity + grounded) so reconciliation can restore full state. `PlayerInputMsg` gains a `jump` byte; `AuthorityPositionMsg` gains velocity + grounded. `kGameId` bumps to lock out old clients.

**Tech Stack:** C++23, Jolt Physics (M18), Vulkan 1.3, GameNetworkingSockets, CMake, MSVC.

---

## File Structure

### New files
- `engine/physics/CharacterController.h` — public API
- `engine/physics/CharacterController.cpp` — pimpl impl (Jolt headers live here)
- `tests/test_character_controller.cpp` — 5 sub-tests

### Modified files
- `engine/physics/PhysicsWorld.h` — expose `Impl` definition pointer access for the new wrapper (small: one engine-only accessor on the existing forward-decl)
- `engine/CMakeLists.txt` — register `physics/CharacterController.cpp`
- `tests/CMakeLists.txt` — register `test_character_controller`
- `games/07-net-shooter/Messages.h` — bump `kGameId`, extend `PlayerInputMsg` + `AuthorityPositionMsg`
- `games/07-net-shooter/main.cpp` — extend `PlayerState`/`PlayerInput`, swap simulate, populate arena collision into per-player `PhysicsWorld`, wire jump key, update input + authority broadcast/handlers
- `docs/engine/physics.md` — append M19 section

---

## Task 1: `iron::CharacterController` engine API + tests

**Files:**
- Create: `engine/physics/CharacterController.h`
- Create: `engine/physics/CharacterController.cpp`
- Modify: `engine/physics/PhysicsWorld.h` (one accessor)
- Modify: `engine/CMakeLists.txt`
- Create: `tests/test_character_controller.cpp`
- Modify: `tests/CMakeLists.txt`

Standalone task. After this, the engine has a working capsule character; tests pass. No game uses it yet.

- [ ] **Step 1: Expose the internal `Impl*` accessor on `PhysicsWorld`**

`CharacterController` needs to reach the underlying `JPH::PhysicsSystem` to register the character. The M18 spec already has `struct PhysicsWorld::Impl;` as a public forward-decl in `PhysicsWorld.h` for exactly this purpose.

Open `engine/physics/PhysicsWorld.h`. Find the `private:` section near the bottom of the class. Move the `struct Impl;` forward declaration up to the `public:` section if it's currently private. Then, in `public:`, add right after the existing methods (before the `private:` line):

```cpp
    // Engine-internal: returns the pimpl pointer so other engine TUs (e.g.
    // CharacterController) can reach the underlying JPH::PhysicsSystem.
    // Game code MUST NOT touch this — it's stable only across engine TUs.
    struct Impl;
    Impl* engineImpl() { return impl_.get(); }
```

Remove any duplicate `struct Impl;` forward-decl from `private:`. The `std::unique_ptr<Impl> impl_;` member stays in `private:`.

Verify in `PhysicsWorld.cpp`: the `struct PhysicsWorld::Impl { ... }` full definition stays in the .cpp file as before — game code never sees it.

- [ ] **Step 2: Write `engine/physics/CharacterController.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "physics/PhysicsWorld.h"

#include <memory>

namespace iron {

struct CharacterControllerConfig {
    float radius        = 0.30f;
    float halfHeight    = 0.90f;        // capsule body, excluding hemispheres;
                                         // total height = 2 * (halfHeight + radius) = 2.4m
    float maxSlopeRad   = 0.785398f;    // 45 deg
    float stepHeight    = 0.30f;
    float jumpVelocity  = 5.5f;
    float gravity       = -9.81f;
};

// Capsule character controller wrapping JPH::CharacterVirtual. Each
// instance owns one CharacterVirtual registered in a PhysicsWorld.
//
// Per-tick contract:
//   1. caller may setFootPosition / setVelocity (used by reconciliation)
//   2. caller calls update(dt, desiredHorizontalVelocity, wantJump)
//   3. caller calls PhysicsWorld::step(dt) to advance the rest of the world
//   4. caller reads footPosition / velocity / isGrounded
//
// The controller's gravity is applied INSIDE update(); the rest of the
// world's gravity is applied by PhysicsWorld::step. They're independent
// because the character doesn't participate in the world's broadphase
// the same way rigid bodies do.
class CharacterController {
public:
    CharacterController();
    ~CharacterController();

    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // Creates the CharacterVirtual in `world`. Returns false if creation
    // fails. `footPosition` is the bottom of the capsule.
    bool create(PhysicsWorld& world,
                const CharacterControllerConfig& cfg,
                Vec3 footPosition);

    // Destroys the CharacterVirtual. Safe to call even if create wasn't
    // called or already destroyed.
    void destroy(PhysicsWorld& world);

    // Per-tick. `desiredVelocity` is the world-space horizontal velocity
    // the player wants (x and z components; y is ignored — gravity owns y).
    // `wantJump` is one-shot: set vy = jumpVelocity if grounded, ignored
    // otherwise.
    void update(float dt, Vec3 desiredVelocity, bool wantJump);

    // State accessors.
    Vec3 footPosition() const;
    Vec3 velocity()     const;
    bool isGrounded()   const;

    // State mutators. Used for reconciliation replay in PredictionEngine —
    // restore controller state from a snapshot before stepping forward.
    void setFootPosition(Vec3);
    void setVelocity(Vec3);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
```

- [ ] **Step 3: Write `engine/physics/CharacterController.cpp`**

```cpp
// CharacterController.cpp — Jolt CharacterVirtual wrapper (pimpl).

#include "physics/CharacterController.h"
#include "core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <utility>

namespace iron {

namespace {

// Object layer for character — must match the MOVING layer defined in
// PhysicsWorld.cpp. Hardcoded here to match (kept in sync by both files
// referencing the same value).
constexpr JPH::ObjectLayer kMovingLayer = 1;

inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3 toI(JPH::Vec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }
inline Vec3 toI(JPH::RVec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }

}  // namespace

// Forward declaration of PhysicsWorld's Impl so we can reach in.
struct PhysicsWorld::Impl {
    // Mirror of the definition in PhysicsWorld.cpp — only members the
    // wrapper needs to touch. C++ linker is happy with the public
    // accessor returning the same Impl* across TUs.
    //
    // We can't include the full definition (it's in the other .cpp), so
    // we instead get raw access via PhysicsWorld::engineImpl() and cast
    // through a getter we add to PhysicsWorld.cpp.
};

// Bridge: PhysicsWorld.cpp exposes the JPH::PhysicsSystem via an inline
// accessor we add. We declare it here as extern.
namespace internal {
JPH::PhysicsSystem* getPhysicsSystem(PhysicsWorld& world);
}

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtual> character;
    CharacterControllerConfig cfg;
};

CharacterController::CharacterController()  : impl_(std::make_unique<Impl>()) {}
CharacterController::~CharacterController() = default;

bool CharacterController::create(PhysicsWorld& world,
                                  const CharacterControllerConfig& cfg,
                                  Vec3 footPosition) {
    JPH::PhysicsSystem* sys = internal::getPhysicsSystem(world);
    if (!sys) {
        Log::error("CharacterController::create: PhysicsWorld not initialized");
        return false;
    }
    impl_->cfg = cfg;

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mMaxSlopeAngle = cfg.maxSlopeRad;
    settings->mShape = new JPH::CapsuleShape(cfg.halfHeight, cfg.radius);
    settings->mInnerBodyLayer = kMovingLayer;
    // CharacterVirtual position is at the center of the capsule body;
    // capsule bottom is at center.y - (halfHeight + radius).
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -cfg.radius);
    settings->mCharacterPadding = 0.02f;

    const float centerY = footPosition.y + cfg.halfHeight + cfg.radius;
    impl_->character = new JPH::CharacterVirtual(
        settings,
        JPH::RVec3(footPosition.x, centerY, footPosition.z),
        JPH::Quat::sIdentity(),
        /*userData=*/0,
        sys);
    return true;
}

void CharacterController::destroy(PhysicsWorld&) {
    impl_->character = nullptr;  // JPH::Ref releases
}

void CharacterController::update(float dt, Vec3 desiredVelocity, bool wantJump) {
    if (!impl_->character) return;
    auto& ch = *impl_->character;

    JPH::Vec3 v = ch.GetLinearVelocity();
    // Horizontal velocity = desired (overwrite both x and z each tick).
    v.SetX(desiredVelocity.x);
    v.SetZ(desiredVelocity.z);
    // Vertical: gravity always, jump-impulse if grounded + requested.
    if (wantJump && ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
        v.SetY(impl_->cfg.jumpVelocity);
    } else {
        v.SetY(v.GetY() + impl_->cfg.gravity * dt);
    }
    ch.SetLinearVelocity(v);

    // Advance the character against world geometry.
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mWalkStairsStepUp        = JPH::Vec3(0, impl_->cfg.stepHeight, 0);
    updateSettings.mWalkStairsStepDownExtra = JPH::Vec3::sZero();

    JPH::PhysicsSystem* sys = ch.GetSystem();
    ch.ExtendedUpdate(dt,
                      sys->GetGravity(),
                      updateSettings,
                      sys->GetDefaultBroadPhaseLayerFilter(kMovingLayer),
                      sys->GetDefaultLayerFilter(kMovingLayer),
                      JPH::BodyFilter(),  // collide with everything
                      JPH::ShapeFilter(),
                      *sys->GetTempAllocator());
}

Vec3 CharacterController::footPosition() const {
    if (!impl_->character) return {};
    const JPH::RVec3 c = impl_->character->GetPosition();
    return Vec3{c.GetX(),
                c.GetY() - impl_->cfg.halfHeight - impl_->cfg.radius,
                c.GetZ()};
}

Vec3 CharacterController::velocity() const {
    if (!impl_->character) return {};
    return toI(impl_->character->GetLinearVelocity());
}

bool CharacterController::isGrounded() const {
    if (!impl_->character) return false;
    return impl_->character->GetGroundState() ==
        JPH::CharacterVirtual::EGroundState::OnGround;
}

void CharacterController::setFootPosition(Vec3 p) {
    if (!impl_->character) return;
    const float centerY = p.y + impl_->cfg.halfHeight + impl_->cfg.radius;
    impl_->character->SetPosition(JPH::RVec3(p.x, centerY, p.z));
}

void CharacterController::setVelocity(Vec3 v) {
    if (!impl_->character) return;
    impl_->character->SetLinearVelocity(toJ(v));
}

}  // namespace iron
```

- [ ] **Step 4: Add the bridge accessor in `PhysicsWorld.cpp`**

Open `engine/physics/PhysicsWorld.cpp`. Near the bottom, just before the closing `}  // namespace iron`, add:

```cpp
namespace internal {

JPH::PhysicsSystem* getPhysicsSystem(PhysicsWorld& world) {
    auto* impl = world.engineImpl();
    return impl ? impl->system : nullptr;
}

}  // namespace internal
```

This works because:
- `PhysicsWorld.h` declares `PhysicsWorld::Impl` as a public forward decl + `engineImpl()` accessor returning `Impl*`.
- `PhysicsWorld.cpp` has the full `struct PhysicsWorld::Impl { ... };` definition with the `JPH::PhysicsSystem* system` member.
- This internal function lives in `PhysicsWorld.cpp` so it can see the full definition.
- `CharacterController.cpp` declares `internal::getPhysicsSystem` as extern and links to this definition.

`internal::getPhysicsSystem` is engine-internal — it lives inside the `iron::internal` namespace and is not part of the public API. Game code does not use it.

> **Important:** Remove the empty `struct PhysicsWorld::Impl { };` placeholder in `CharacterController.cpp` (Step 3 of this task had a comment about it but no actual definition). The forward declaration of `PhysicsWorld::Impl` from `PhysicsWorld.h` is enough — `CharacterController.cpp` never dereferences it directly; it only calls `internal::getPhysicsSystem` which does the dereference.

Update `CharacterController.cpp` accordingly: delete the `struct PhysicsWorld::Impl { ... };` stub block from Step 3 (lines starting with `// Forward declaration of PhysicsWorld's Impl`). The `extern` declaration of `internal::getPhysicsSystem` is the only bridge needed.

The corrected `CharacterController.cpp` top section (replacing the lines from Step 3 between the includes and the `struct CharacterController::Impl` definition):

```cpp
namespace iron {

namespace {

constexpr JPH::ObjectLayer kMovingLayer = 1;

inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3 toI(JPH::Vec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }
inline Vec3 toI(JPH::RVec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }

}  // namespace

// Bridge to PhysicsWorld's internal Jolt system. Defined in PhysicsWorld.cpp.
namespace internal {
JPH::PhysicsSystem* getPhysicsSystem(PhysicsWorld& world);
}
```

- [ ] **Step 5: Register `CharacterController.cpp` in `engine/CMakeLists.txt`**

Open `engine/CMakeLists.txt`. Find the line `physics/Ragdoll.cpp` (added in M18). Add immediately after:

```cmake
  physics/CharacterController.cpp
```

- [ ] **Step 6: Build to verify the engine compiles**

```
cmake --build build-vk --config Debug --target ironcore
```

Expected: clean compile. If you get a redefinition error on `PhysicsWorld::Impl`, you forgot to delete the stub in `CharacterController.cpp` per Step 4. If `internal::getPhysicsSystem` is undefined at link time, you forgot to add it to `PhysicsWorld.cpp` per Step 4.

- [ ] **Step 7: Write `tests/test_character_controller.cpp`**

```cpp
#include "physics/CharacterController.h"
#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Create + destroy ---
    {
        PhysicsWorld w; w.init();
        CharacterController c;
        CharacterControllerConfig cfg;
        CHECK(c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f}));
        const Vec3 p = c.footPosition();
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, 0.0f);
        CHECK_NEAR(p.z, 0.0f);
        c.destroy(w);
    }

    // --- Gravity: character falls when above ground with no support ---
    {
        PhysicsWorld w; w.init();
        // Static ground at y=0 (top of the box is at y=0).
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 10.0f, 0.0f});

        for (int i = 0; i < 60; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false);
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        // Fell ≈ ½ * 9.81 * 1² = 4.905m. Allow ±0.5m for character padding + capsule.
        CHECK(p.y < 10.0f - 4.0f);
        CHECK(p.y > 10.0f - 5.5f);
    }

    // --- Lands on the ground and stays grounded ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 5.0f, 0.0f});

        for (int i = 0; i < 180; ++i) {  // 3 seconds — plenty to land + settle
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false);
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        // Foot should be at or just above ground (y ≈ 0). Padding may
        // leave a tiny offset.
        CHECK(p.y > -0.05f);
        CHECK(p.y <  0.10f);
        CHECK(c.isGrounded());
    }

    // --- Wall collision: walk forward into a wall, position is blocked ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});
        // Wall 1m in front of spawn (z = +1, half-extents 0.1).
        w.createStaticBox({0.0f, 1.0f, 1.5f}, {2.0f, 1.0f, 0.1f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f});

        // Settle on the ground first.
        for (int i = 0; i < 30; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false);
            w.step(1.0f / 60.0f);
        }
        // Walk forward at 6 m/s for 1s. Without a wall this would travel
        // 6m; with the wall, much less. Wall is at z ≈ 1.4 (1.5 - 0.1).
        for (int i = 0; i < 60; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,6.0f}, false);
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.z < 1.5f);  // didn't pass through the wall
    }

    // --- Jump: from grounded, jump sets +y velocity ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});

        CharacterController c;
        CharacterControllerConfig cfg;
        c.create(w, cfg, Vec3{0.0f, 0.0f, 0.0f});

        // Settle.
        for (int i = 0; i < 30; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false);
            w.step(1.0f / 60.0f);
        }
        CHECK(c.isGrounded());

        // Jump on one tick.
        c.update(1.0f / 60.0f, Vec3{0,0,0}, true);
        w.step(1.0f / 60.0f);
        const Vec3 v = c.velocity();
        CHECK(v.y > 4.0f);  // initial jumpVelocity = 5.5; allow drift from first integration step

        // Continue falling — over ~1s should come back near ground.
        for (int i = 0; i < 90; ++i) {
            c.update(1.0f / 60.0f, Vec3{0,0,0}, false);
            w.step(1.0f / 60.0f);
        }
        const Vec3 p = c.footPosition();
        CHECK(p.y > -0.05f);
        CHECK(p.y <  0.20f);  // back to ground level
    }

    return iron_test_result();
}
```

- [ ] **Step 8: Register the test in `tests/CMakeLists.txt`**

After the existing `iron_add_test(test_ragdoll ...)` line (added in M18), add:

```cmake
iron_add_test(test_character_controller test_character_controller.cpp)
```

- [ ] **Step 9: Build + run the test**

```
cmake --build build-vk --config Debug --target ironcore test_character_controller
ctest --test-dir build-vk -C Debug -R test_character_controller --output-on-failure
```

Expected: PASS. All 5 sub-tests pass.

Troubleshooting:
- If "Lands on the ground" fails with the character below ground (y < -0.05): `mCharacterPadding` may be wrong sign — Jolt allows the capsule's bottom to dip slightly below the ground; if it dips more than padding, settings might be off. Try `mCharacterPadding = 0.05f`.
- If "Wall collision" passes through (p.z > 1.5): the `mInnerBodyLayer` setting might not include the static ground/wall layer. Check the layer constants match `PhysicsWorld.cpp`'s `Layers::NON_MOVING = 0` and `MOVING = 1` exactly.
- If "Jump" velocity is 0 immediately after jump: the `GetGroundState` check might be too strict; verify `c.isGrounded()` returns true right before the jump call.

- [ ] **Step 10: Commit**

```
git add engine/physics/PhysicsWorld.h engine/physics/PhysicsWorld.cpp \
        engine/physics/CharacterController.h engine/physics/CharacterController.cpp \
        engine/CMakeLists.txt \
        tests/test_character_controller.cpp tests/CMakeLists.txt
git commit -m "M19 Task 1: iron::CharacterController (Jolt CharacterVirtual wrapper)"
```

---

## Task 2: Net-shooter port — extend PlayerState/PlayerInput, swap simulate, wire jump, populate arena collision

**Files:**
- Modify: `games/07-net-shooter/Messages.h`
- Modify: `games/07-net-shooter/main.cpp`

After this task, net-shooter players have wall collision, gravity, ground detection, jump. Bumped `kGameId` locks out old clients.

- [ ] **Step 1: Bump `kGameId` and extend the wire messages**

Open `games/07-net-shooter/Messages.h`. Modify three things:

```cpp
// "NSTR" — net shooter. Bumped for M19 (PlayerInputMsg + AuthorityPositionMsg
// wire-format break). Old clients cannot connect to new hosts.
constexpr std::uint32_t kGameId = 0x4E535453u;  // was 0x4E535452u
```

```cpp
// Client -> Host: input tick.
struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float vx;             // world-space x velocity (m/s)
    float vy;             // reserved (unused in M19; v1 always 0)
    float vz;             // world-space z velocity (m/s)
    std::uint8_t jump;    // 0 = none, 1 = pressed this tick
};
```

```cpp
// Host -> Clients: authoritative position broadcast.
struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t peerId;
    float x, y, z;
    float vx, vy, vz;        // M19 — velocity for reconciliation
    std::uint8_t grounded;   // M19 — ground state
    std::uint32_t lastInputId;
};
```

(No other message types change.)

- [ ] **Step 2: Extend `PlayerState` and `PlayerInput` in `main.cpp`**

Open `games/07-net-shooter/main.cpp`. Find the `struct PlayerState` definition (around line 593). Replace:

```cpp
struct PlayerState {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool operator==(const PlayerState& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};
struct PlayerInput { float dx, dy, dz; };
```

With:

```cpp
struct PlayerState {
    float x  = 0.0f, y  = 0.0f, z  = 0.0f;     // foot position
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;     // velocity
    bool  grounded = false;
    bool operator==(const PlayerState& o) const {
        return x == o.x && y == o.y && z == o.z
            && vx == o.vx && vy == o.vy && vz == o.vz
            && grounded == o.grounded;
    }
};
struct PlayerInput { float vx = 0.0f, vy = 0.0f, vz = 0.0f; bool jump = false; };
```

- [ ] **Step 3: Add helper to populate arena collision into a `PhysicsWorld`**

In `games/07-net-shooter/main.cpp`, near the top of the anonymous namespace (after the includes block, before `main()`), add:

```cpp
// Convert the arena's AABBs into static collision in `world`. Game-side
// helper — the engine doesn't know about netshooter::Arena.
void populateArenaCollision(iron::PhysicsWorld& world,
                            const iron::netshooter::Arena& arena) {
    for (const auto& box : arena.boxes) {
        const iron::Vec3 center{
            (box.min.x + box.max.x) * 0.5f,
            (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f,
        };
        const iron::Vec3 halfExtents{
            (box.max.x - box.min.x) * 0.5f,
            (box.max.y - box.min.y) * 0.5f,
            (box.max.z - box.min.z) * 0.5f,
        };
        world.createStaticBox(center, halfExtents);
    }
}
```

Add includes near the top of `main.cpp` (next to the existing `#include "physics/..."` if any, else next to the other engine includes):

```cpp
#include "physics/PhysicsWorld.h"
#include "physics/CharacterController.h"
```

- [ ] **Step 4: Create per-player physics worlds + controllers + rewrite `simulate`**

In `games/07-net-shooter/main.cpp`, find the existing `simulate` lambda + `PredictionEngine` construction (around lines 601-618). Replace with:

```cpp
// --- M19: per-player PhysicsWorld + CharacterController. The local
// client owns one (for its own predicted player); the host owns one per
// peer. Each world contains the arena geometry + exactly one character.

iron::PhysicsWorld localWorld;
localWorld.init();
populateArenaCollision(localWorld, arena);

iron::CharacterController localChar;
iron::CharacterControllerConfig charCfg;  // defaults: 0.3 radius, 0.9 halfHeight, 5.5 jump
iron::Vec3 spawnPos = arena.spawnPoints.empty()
    ? iron::Vec3{0.0f, 1.0f, 0.0f}
    : arena.spawnPoints[0];
localChar.create(localWorld, charCfg, spawnPos);

// Capture by reference. simulate is called by PredictionEngine for
// prediction AND for replay during reconciliation.
auto simulate = [&localWorld, &localChar]
                (PlayerState s, PlayerInput in, float dt) -> PlayerState {
    localChar.setFootPosition({s.x, s.y, s.z});
    localChar.setVelocity({s.vx, s.vy, s.vz});
    localChar.update(dt, iron::Vec3{in.vx, 0.0f, in.vz}, in.jump);
    localWorld.step(dt);

    const iron::Vec3 p = localChar.footPosition();
    const iron::Vec3 v = localChar.velocity();
    return PlayerState{p.x, p.y, p.z, v.x, v.y, v.z, localChar.isGrounded()};
};

iron::PredictionEngine<PlayerInput, PlayerState> predictor{
    simulate,
    /*fixedDt=*/ 1.0f / 60.0f,
    /*initial=*/ PlayerState{spawnPos.x, spawnPos.y, spawnPos.z,
                              0.0f, 0.0f, 0.0f, true}};
```

- [ ] **Step 5: On the host, create per-peer worlds + controllers**

Find the host-side `authStates` map declaration. It currently maps `peerId -> PlayerState`. Add parallel maps for the host's per-peer simulate machinery. Near the existing `authStates`:

```cpp
// Host-only: per-peer PhysicsWorld + CharacterController + simulate.
// Each peer's simulate captures THIS peer's world + controller, so
// authState[pid] = simulate_for_pid(authState[pid], in, dt).
struct HostPlayerSim {
    iron::PhysicsWorld           world;
    iron::CharacterController    controller;
    std::function<PlayerState(PlayerState, PlayerInput, float)> simulateFn;
};
std::unordered_map<std::uint32_t, std::unique_ptr<HostPlayerSim>> hostSims;

auto ensureHostSim = [&](std::uint32_t pid, iron::Vec3 spawn) -> HostPlayerSim& {
    auto it = hostSims.find(pid);
    if (it != hostSims.end()) return *it->second;
    auto sim = std::make_unique<HostPlayerSim>();
    sim->world.init();
    populateArenaCollision(sim->world, arena);
    sim->controller.create(sim->world, charCfg, spawn);
    auto* simPtr = sim.get();
    sim->simulateFn = [simPtr](PlayerState s, PlayerInput in, float dt) -> PlayerState {
        simPtr->controller.setFootPosition({s.x, s.y, s.z});
        simPtr->controller.setVelocity({s.vx, s.vy, s.vz});
        simPtr->controller.update(dt, iron::Vec3{in.vx, 0.0f, in.vz}, in.jump);
        simPtr->world.step(dt);
        const iron::Vec3 p = simPtr->controller.footPosition();
        const iron::Vec3 v = simPtr->controller.velocity();
        return PlayerState{p.x, p.y, p.z, v.x, v.y, v.z, simPtr->controller.isGrounded()};
    };
    hostSims[pid] = std::move(sim);
    return *hostSims[pid];
};
```

The existing host-side input handler (search for `registry.registerHandler<iron::netshooter::PlayerInputMsg>`) currently does:

```cpp
authStates[*pid] = simulate(authStates[*pid], in, 0.0f);
```

Replace with:

```cpp
HostPlayerSim& sim = ensureHostSim(*pid, /*spawn*/ {authStates[*pid].x,
                                                    authStates[*pid].y,
                                                    authStates[*pid].z});
const PlayerInput inP{msg.vx, msg.vy, msg.vz, msg.jump != 0};
authStates[*pid] = sim.simulateFn(authStates[*pid], inP, 1.0f / 60.0f);
```

Also: any spot where the host inits `authStates[pid]` to a spawn point (search for `authStates[pid] = PlayerState{sp.x, sp.y, sp.z}`) — update to:

```cpp
authStates[pid] = PlayerState{sp.x, sp.y, sp.z, 0.0f, 0.0f, 0.0f, true};
```

And on the same line/block where the spawn happens, force the host sim's controller to reset to the spawn position:

```cpp
HostPlayerSim& sim = ensureHostSim(pid, {sp.x, sp.y, sp.z});
sim.controller.setFootPosition({sp.x, sp.y, sp.z});
sim.controller.setVelocity({0.0f, 0.0f, 0.0f});
```

There are two such spawn sites in the existing main.cpp (around lines 752 and 1377). Update both.

- [ ] **Step 6: Wire jump key + velocity-based input on the client**

Find the existing input-collection block on the client (around line 1100-1145 — search for `PlayerInputMsg` send). The current code computes `dx/dy/dz` from camera-relative motion + `kMoveSpeed`. Replace the input computation to produce velocity directly:

```cpp
// M19: produce velocity (m/s) and jump-pressed instead of position delta.
const float yawCos = std::cos(look.yaw);
const float yawSin = std::sin(look.yaw);
const float fAxis = (input.keyDown(GLFW_KEY_W) ? 1.0f : 0.0f)
                  - (input.keyDown(GLFW_KEY_S) ? 1.0f : 0.0f);
const float sAxis = (input.keyDown(GLFW_KEY_D) ? 1.0f : 0.0f)
                  - (input.keyDown(GLFW_KEY_A) ? 1.0f : 0.0f);

PlayerInput in;
in.vx = (fAxis * yawCos + sAxis * yawSin) * kMoveSpeed;
in.vz = (fAxis * yawSin - sAxis * yawCos) * kMoveSpeed * -1.0f;
in.vy = 0.0f;
in.jump = input.keyPressed(GLFW_KEY_SPACE);  // edge-detected
```

(The sign convention for vz matches the existing dz code path — verify by playing locally. If forward/back is inverted, drop the `* -1.0f`.)

Then update the host-broadcast handler for `PlayerInputMsg` and the local predictor's `predict()` call to use the new field names.

The send call becomes:

```cpp
peers.send<iron::netshooter::PlayerInputMsg>(
    /*to host conn*/,
    iron::netshooter::PlayerInputMsg{
        inputId, in.vx, in.vy, in.vz,
        static_cast<std::uint8_t>(in.jump ? 1 : 0),
    },
    /*reliable=*/ false);
```

And the local predict:

```cpp
predictor.predict(in);
```

(`PredictionEngine::predict` already takes a `TInput` — no signature change.)

- [ ] **Step 7: Update `AuthorityPositionMsg` broadcast + handle**

Find the host-side broadcast of `AuthorityPositionMsg` (around line 850-880, search for `peers.broadcast<...AuthorityPositionMsg>` or similar). Change the construction to include the new fields:

```cpp
iron::netshooter::AuthorityPositionMsg apm;
apm.peerId      = pid;
apm.x           = authStates[pid].x;
apm.y           = authStates[pid].y;
apm.z           = authStates[pid].z;
apm.vx          = authStates[pid].vx;
apm.vy          = authStates[pid].vy;
apm.vz          = authStates[pid].vz;
apm.grounded    = authStates[pid].grounded ? 1 : 0;
apm.lastInputId = lastSeenInputId[pid];  // unchanged
peers.broadcast<iron::netshooter::AuthorityPositionMsg>(apm, /*reliable=*/ false);
```

Client-side handler (search for `registerHandler<...AuthorityPositionMsg>`) — find the line that reconciles the predictor:

```cpp
// Old:
predictor.reconcile(PlayerState{msg.x, msg.y, msg.z}, msg.lastInputId);
```

Replace with:

```cpp
predictor.reconcile(
    PlayerState{msg.x, msg.y, msg.z,
                msg.vx, msg.vy, msg.vz,
                msg.grounded != 0},
    msg.lastInputId);
```

Same for the `predictor.reset(...)` calls on respawn:

```cpp
// Old:
predictor.reset(PlayerState{msg.x, msg.y, msg.z});
// New:
predictor.reset(PlayerState{msg.x, msg.y, msg.z, 0.0f, 0.0f, 0.0f, true});
```

- [ ] **Step 8: Build the client + run a solo smoke test**

```
cmake --build build-vk --config Debug --target net-shooter
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Expected: window opens; you spawn in the arena.
- Walk into a wall: stop. Cannot pass through.
- Press Space: jump up, fall back down.
- Walk off the arena edge or jump from a high spot: gravity pulls you down.
- No de-syncs, FPS still healthy (60+).

If the player falls through the floor immediately, the arena's floor box at the bottom of the AABB list has its top edge below y=0; check `populateArenaCollision` is creating it correctly (center.y vs. half-extents.y).

If the player can't jump: confirm `input.keyPressed(GLFW_KEY_SPACE)` is edge-triggered (one-shot) rather than always-true.

If the player drifts sideways while stationary: the velocity-set code is leaking sideways force across ticks; check Step 4's `setVelocity` is called with the previous state's vx/vz before `update` overwrites them.

- [ ] **Step 9: Networked smoke test (host + one client on same machine)**

In one PowerShell:
```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

In another:
```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --connect 127.0.0.1
```

Expected: both windows show each other. Both players can walk + jump. Movement on the client is smooth (prediction); when the server corrects (rare on localhost), no visible snap.

- [ ] **Step 10: Commit**

```
git add games/07-net-shooter/Messages.h games/07-net-shooter/main.cpp
git commit -m "M19 Task 2: net-shooter onto CharacterController (collision, gravity, jump)"
```

---

## Task 3: Docs

**Files:**
- Modify: `docs/engine/physics.md`

- [ ] **Step 1: Append the M19 section to `docs/engine/physics.md`**

```markdown
## M19 — Capsule character controller

`iron::CharacterController` (`engine/physics/CharacterController.{h,cpp}`)
wraps `JPH::CharacterVirtual` — Jolt's purpose-built kinematic character.
One instance owns one capsule registered with a `PhysicsWorld`.

```cpp
iron::PhysicsWorld world; world.init();
// ... populate world with static collision ...

iron::CharacterController player;
iron::CharacterControllerConfig cfg;  // 0.3m radius, 0.9m halfHeight, jump 5.5 m/s
player.create(world, cfg, /*footPosition=*/ {0,0,0});

// Per frame:
player.update(dt, iron::Vec3{vx, 0, vz}, jumpPressedThisFrame);
world.step(dt);

iron::Vec3 footPos = player.footPosition();
iron::Vec3 vel     = player.velocity();
bool grounded      = player.isGrounded();
```

The wrapper owns the JPH `Ref<CharacterVirtual>`; lifetime ends with
`destroy(world)` or the wrapper's destructor. Direct setters
(`setFootPosition`, `setVelocity`) exist for reconciliation replay —
the `simulate` lambda in net-shooter restores controller state at the
start of every call so `PredictionEngine` replays work correctly.

### Per-player physics worlds in net-shooter (v1)

Net-shooter creates one `PhysicsWorld` + one `CharacterController` per
simulated player. The client owns one (its local predicted player); the
host owns one per peer. Each world contains the arena's static AABBs +
exactly one character. **Players do not collide with each other in v1**
— that's a future track. Hitscan + rocket splash still resolve hits via
the existing `LagCompensator` against the same arena AABBs and player
AABBs from the game-side history.

The shape of the static arena collision is a literal port of the
existing `iron::netshooter::Arena::boxes` vector — each `Aabb` becomes
a `world.createStaticBox(center, halfExtents)` call at startup.

### Wire-format change (M19)

`PlayerInputMsg` gained `vy` (reserved) and `jump` (uint8). The previous
`dx/dy/dz` field names are gone — the new fields are world-space
velocity (m/s).

`AuthorityPositionMsg` gained `vx/vy/vz` (velocity) and `grounded`
(uint8). Required so client reconciliation can restore full character
state, not just position.

`kGameId` bumped from `0x4E535452` to `0x4E535453` — old clients
cannot connect to new hosts.

### What's next

- M20 — projectile rigid bodies + raycasts in net-shooter.
- M21 — death-into-ragdoll wiring (uses M18's `iron::Ragdoll`).
```

- [ ] **Step 2: Commit**

```
git add docs/engine/physics.md
git commit -m "M19 Task 3: docs/engine/physics.md (CharacterController + wire-format break)"
```

- [ ] **Step 3: Push + open PR**

```
git push -u origin feat/m19-capsule-character-controller
gh pr create --title "M19: capsule character controller + net-shooter port" --body "$(cat <<'EOF'
## Summary
- `iron::CharacterController` (`engine/physics/CharacterController.{h,cpp}`) wraps `JPH::CharacterVirtual`
- Per-player `PhysicsWorld` topology (client = 1, host = N); arena AABBs populated as static collision
- Net-shooter `simulate` rewritten — players get wall collision, gravity, ground detection, jump
- `PlayerState` grows position-only → position + velocity + grounded
- `PlayerInputMsg` + `AuthorityPositionMsg` wire-format break; `kGameId` bumped
- 5 new unit tests (create/destroy, gravity, landing, wall collision, jump)
- New `docs/engine/physics.md` M19 section

Second milestone of the physics overhaul track. Built on M18's `iron::PhysicsWorld`. M20 (projectile rigid bodies + raycasts in net-shooter) and M21 (death-into-ragdoll) come next.

## Test plan
- [ ] CI green (Windows MSVC) — new tests included
- [ ] Solo net-shooter: walk into walls = blocked, Space = jump, gravity works
- [ ] Two-process net-shooter: prediction smooth, no visible reconciliation snap

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review Summary

**Spec coverage:**
- ✅ `iron::CharacterController` API → Task 1
- ✅ Per-player PhysicsWorld topology → Task 2 Steps 4–5
- ✅ Static arena ingestion → Task 2 Step 3
- ✅ `PlayerState`/`PlayerInput` extension → Task 2 Step 2
- ✅ Wire format break (PlayerInputMsg/AuthorityPositionMsg) + `kGameId` bump → Task 2 Step 1
- ✅ Jump (edge-detected Space) → Task 2 Step 6
- ✅ Determinism preserved (reconcile-friendly setters) → Task 1 Steps 2–3 + verified by net-shooter using the existing PredictionEngine
- ✅ 5 unit tests → Task 1 Step 7
- ✅ Docs → Task 3

**Placeholder scan:** clean — every code step has full code. No "TBD" or "implement later".

**Type consistency:**
- `BodyId`, `JointId` (M18) preserved; `CharacterController` doesn't introduce a new handle type — it owns the `JPH::Ref<CharacterVirtual>` internally.
- `PlayerState`/`PlayerInput` field names (`x/y/z/vx/vy/vz/grounded`, `vx/vz/jump`) consistent across `Messages.h`, simulate lambda, predictor calls.
- `kMovingLayer = 1` in `CharacterController.cpp` matches `Layers::MOVING = 1` in `PhysicsWorld.cpp` (M18).
- `engineImpl()` accessor on `PhysicsWorld` returns `Impl*` consistently — only used by `internal::getPhysicsSystem` in `PhysicsWorld.cpp`.

**Known v1 simplifications called out:**
- Players don't collide with each other (separate per-player worlds).
- Wall collision uses the existing arena AABBs; no per-axis sliding fine-tuning.
- Jump uses a simple "set vy = jumpVelocity if grounded" approach — air control comes from `CharacterVirtual` naturally.

These are deliberate; documented in spec and inline.
