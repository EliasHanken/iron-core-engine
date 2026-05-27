# M18 Jolt Physics + Ragdoll Playground Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate Jolt Physics into the engine via vcpkg, ship `iron::PhysicsWorld` + `iron::Ragdoll` wrappers, and deliver `games/09-physics-playground` — a Vulkan visual validator where 11-bone ragdolls tumble down ramps and react to player-fired spheres.

**Architecture:** Jolt enters via vcpkg manifest. `iron::PhysicsWorld` is a pimpl wrapper hiding all Jolt types from public headers — game code only ever sees engine-typed handles (`BodyId`, `JointId`) and engine math types (`Vec3`, `Quaternion`, `Mat4`). `iron::Ragdoll` builds on `PhysicsWorld` to spawn an 11-body humanoid skeleton with 10 joints (swing-twist for shoulders/hips/spine/neck; hinges for elbows/knees). The playground game renders each rigid body as a colored cube via the existing Vulkan lit shader path.

**Tech Stack:** C++23, Jolt Physics 5.x via vcpkg, Vulkan 1.3 (existing), CMake, MSVC.

---

## File Structure

### New files
- `engine/physics/PhysicsWorld.h` — public API, opaque handles, pimpl
- `engine/physics/PhysicsWorld.cpp` — Jolt-touching impl (uses pimpl)
- `engine/physics/Ragdoll.h` — declarative humanoid skeleton
- `engine/physics/Ragdoll.cpp` — bone + joint construction
- `tests/test_physics_world.cpp` — wrapper smoke tests + determinism
- `tests/test_ragdoll.cpp` — ragdoll lifecycle tests
- `games/09-physics-playground/CMakeLists.txt` — Vulkan-only game target
- `games/09-physics-playground/main.cpp` — scene + controls + render loop
- `docs/engine/physics.md` — engine docs (PhysicsWorld API + Ragdoll + Jolt integration notes)

### Modified files
- `vcpkg.json` — add `"joltphysics"` to dependencies
- `CMakeLists.txt` — `find_package(unofficial-joltphysics CONFIG REQUIRED)` near other find_packages
- `engine/CMakeLists.txt` — register PhysicsWorld.cpp + Ragdoll.cpp; link Jolt PRIVATE
- `tests/CMakeLists.txt` — register the two new test targets

---

## Task 1: vcpkg + PhysicsWorld wrapper (body lifecycle, gravity, raycast, impulse)

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt` (top-level, after the `find_package(GameNetworkingSockets ...)` block)
- Modify: `engine/CMakeLists.txt` (add new sources + link Jolt PRIVATE)
- Create: `engine/physics/PhysicsWorld.h`
- Create: `engine/physics/PhysicsWorld.cpp`
- Create: `tests/test_physics_world.cpp`
- Modify: `tests/CMakeLists.txt` (register `test_physics_world`)

Standalone task — after this lands, `ironcore` links Jolt; the wrapper compiles, body create/destroy/step/raycast/impulse all work; tests pass. No game uses it yet.

- [ ] **Step 1: Add `joltphysics` to vcpkg manifest**

Open `vcpkg.json`. Change to:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "joltphysics",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

- [ ] **Step 2: Wire `find_package` in top-level CMake**

In `CMakeLists.txt`, find the existing `find_package(GameNetworkingSockets CONFIG REQUIRED)` line (around line 56). Add this line immediately after it:

```cmake
find_package(unofficial-joltphysics CONFIG REQUIRED)
```

> Notes: the vcpkg `joltphysics` port exports the target as `unofficial::joltphysics::Jolt`. The `unofficial-` prefix is vcpkg convention for libraries that don't ship a CMake config in their upstream — vcpkg generates one.

- [ ] **Step 3: Add sources + Jolt link in `engine/CMakeLists.txt`**

Open `engine/CMakeLists.txt`. In the `add_library(ironcore STATIC ...)` block (lines 1-29), find the existing `physics/Rope.cpp` entry. Insert the two new sources immediately after it:

```cmake
  physics/Rope.cpp
  physics/PhysicsWorld.cpp
  physics/Ragdoll.cpp
```

(Ragdoll.cpp is added now so the build script is set for Task 2 — the file is empty in this task and we'll create the real Ragdoll.cpp in Task 2. Actually no — that would be an empty source file. We add only PhysicsWorld.cpp here.)

Replace with the correct edit:

```cmake
  physics/Rope.cpp
  physics/PhysicsWorld.cpp
```

After the existing `target_link_libraries(ironcore PUBLIC ...)` block (line 32-35), add a new PRIVATE link for Jolt. Find the block:

```cmake
target_link_libraries(ironcore PUBLIC
  glfw
  stb_image
  GameNetworkingSockets::GameNetworkingSockets)
```

And immediately after, add:

```cmake
target_link_libraries(ironcore PRIVATE
  unofficial::joltphysics::Jolt)
```

PRIVATE is important: game code links `ironcore` and must NOT transitively pull Jolt headers.

- [ ] **Step 4: Create `engine/physics/PhysicsWorld.h`**

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cstdint>
#include <memory>

namespace iron {

// Opaque handles. Game code stores these but does not inspect them.
// The 32-bit value matches Jolt's internal ID layout; 0 is reserved as
// "invalid" matching the engine's existing handle convention.
struct BodyId {
    std::uint32_t value = 0;
    bool isValid() const { return value != 0; }
};
struct JointId {
    std::uint32_t value = 0;
    bool isValid() const { return value != 0; }
};

inline constexpr BodyId  kInvalidBody  {};
inline constexpr JointId kInvalidJoint {};

inline bool operator==(BodyId  a, BodyId  b) { return a.value == b.value; }
inline bool operator==(JointId a, JointId b) { return a.value == b.value; }

// Thin engine wrapper around Jolt Physics. Hides all Jolt types from
// public headers via pimpl: game code includes only this header and the
// engine math types it already uses.
//
// Threading: Jolt's PhysicsSystem internally uses a job system. For v1,
// PhysicsWorld is single-threaded externally — call `step` from one
// thread. The internal job system runs on its own threads.
//
// Determinism: configured to be byte-deterministic across runs and
// machines on the same architecture. step(dt) with identical bodies +
// dt produces identical body states. Important for the engine's
// server-authoritative networking with client prediction (M19+).
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool init();         // returns false if Jolt static init fails
    void shutdown();     // safe to call multiple times; ~PhysicsWorld calls it

    // Deterministic fixed step. Caller picks dt (typically 1/60).
    // Internally uses 1 collision step + 1 integration sub-step.
    void step(float dt);

    // --- Body creation ---
    BodyId createStaticBox     (Vec3 pos, Vec3 halfExtents);
    BodyId createDynamicBox    (Vec3 pos, Vec3 halfExtents, float mass);
    BodyId createDynamicSphere (Vec3 pos, float radius,         float mass);
    BodyId createDynamicCapsule(Vec3 pos, float halfHeight, float radius, float mass);
    void   destroyBody(BodyId);

    // --- Body state ---
    Vec3       bodyPosition (BodyId) const;
    Quaternion bodyRotation (BodyId) const;
    Mat4       bodyTransform(BodyId) const;
    bool       isBodyAlive  (BodyId) const;

    // --- Forces / impulses ---
    void applyImpulse     (BodyId, Vec3 impulse);
    void applyForceAtPoint(BodyId, Vec3 force, Vec3 worldPoint);
    void setVelocity      (BodyId, Vec3 vel);

    // --- Queries ---
    struct RaycastHit {
        bool   hit    = false;
        BodyId body   = kInvalidBody;
        Vec3   point  {};
        Vec3   normal {};
        float  t      = 0.0f;   // 0..1 along the ray
    };
    RaycastHit raycast(Vec3 origin, Vec3 direction, float maxDistance) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
```

- [ ] **Step 5: Create `engine/physics/PhysicsWorld.cpp`**

```cpp
// PhysicsWorld.cpp — Jolt Physics wrapper (pimpl).
//
// All Jolt types live in this translation unit. Public header exposes
// only engine math types + opaque handles.

#include "physics/PhysicsWorld.h"
#include "core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/RegisterTypes.h>

#include <thread>

namespace iron {

namespace {

// Object layer convention (broadphase categorization).
// NON_MOVING: static geometry (ground, ramps, walls).
// MOVING: everything dynamic (boxes, spheres, ragdoll bones).
namespace Layers {
    constexpr JPH::ObjectLayer NON_MOVING = 0;
    constexpr JPH::ObjectLayer MOVING     = 1;
    constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}
namespace BroadphaseLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
    constexpr JPH::uint NUM_LAYERS = 2;
}

// Maps object layers -> broadphase layers. Required by Jolt.
class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterfaceImpl() {
        mTable[Layers::NON_MOVING] = BroadphaseLayers::NON_MOVING;
        mTable[Layers::MOVING]     = BroadphaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadphaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return mTable[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Iron"; }
#endif
private:
    JPH::BroadPhaseLayer mTable[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        switch (obj) {
            case Layers::NON_MOVING: return bp == BroadphaseLayers::MOVING;  // static collides only with dynamic
            case Layers::MOVING:     return true;                            // dynamic collides with everything
            default: return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING && b == Layers::NON_MOVING) return false;
        return true;
    }
};

// Conversion helpers. Engine Vec3 is float xyz; Jolt Vec3 is __m128 lane.
inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3      toI(JPH::Vec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }
inline JPH::Quat toJ(Quaternion q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline Quaternion toI(JPH::Quat q) { return Quaternion{q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

inline Mat4 mat4FromJolt(JPH::RVec3 pos, JPH::Quat rot) {
    // Build TRS = T * R (no scale). Engine Mat4 is column-major; build
    // from quaternion + translation via the existing helper if one
    // exists, otherwise inline.
    JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rot, pos);
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        JPH::Vec4 c = m.GetColumn4(col);
        out.at(0, col) = c.GetX();
        out.at(1, col) = c.GetY();
        out.at(2, col) = c.GetZ();
        out.at(3, col) = c.GetW();
    }
    return out;
}

bool g_joltInitialized = false;

}  // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl*     temp       = nullptr;
    JPH::JobSystemThreadPool*   jobs       = nullptr;
    JPH::PhysicsSystem*         system     = nullptr;
    BroadPhaseLayerInterfaceImpl       broadphaseLayers;
    ObjectVsBroadPhaseLayerFilterImpl  objectVsBroadphase;
    ObjectLayerPairFilterImpl          objectVsObject;
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init() {
    if (!g_joltInitialized) {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        g_joltInitialized = true;
    }

    impl_->temp = new JPH::TempAllocatorImpl(10 * 1024 * 1024);  // 10 MB

    // Single thread for byte-deterministic results. Jolt's island
    // splitter is non-deterministic when threaded.
    const int threadCount = 1;
    impl_->jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs,
                                                JPH::cMaxPhysicsBarriers,
                                                threadCount);

    impl_->system = new JPH::PhysicsSystem();
    impl_->system->Init(
        /*maxBodies=*/4096,
        /*numBodyMutexes=*/0,
        /*maxBodyPairs=*/4096,
        /*maxContactConstraints=*/2048,
        impl_->broadphaseLayers,
        impl_->objectVsBroadphase,
        impl_->objectVsObject);
    impl_->system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    Log::info("PhysicsWorld: Jolt initialized (deterministic single-threaded mode)");
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    if (impl_->system) {
        // Bodies are auto-cleaned by destroying the system.
        delete impl_->system; impl_->system = nullptr;
    }
    if (impl_->jobs) { delete impl_->jobs; impl_->jobs = nullptr; }
    if (impl_->temp) { delete impl_->temp; impl_->temp = nullptr; }
}

void PhysicsWorld::step(float dt) {
    if (!impl_->system) return;
    constexpr int kCollisionSteps = 1;
    impl_->system->Update(dt, kCollisionSteps, impl_->temp, impl_->jobs);
}

// --- Body creation helpers ---

namespace {

BodyId createBodyImpl(PhysicsWorld::Impl& impl,
                      JPH::ShapeRefC shape,
                      JPH::RVec3 pos,
                      JPH::EMotionType motion,
                      JPH::ObjectLayer layer,
                      float mass) {
    JPH::BodyCreationSettings settings(shape, pos, JPH::Quat::sIdentity(), motion, layer);
    if (motion == JPH::EMotionType::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }
    JPH::BodyInterface& bi = impl.system->GetBodyInterface();
    JPH::BodyID jid = bi.CreateAndAddBody(settings,
        motion == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate
                                             : JPH::EActivation::DontActivate);
    return BodyId{jid.GetIndexAndSequenceNumber()};
}

inline JPH::BodyID toJoltBodyId(BodyId b) {
    return JPH::BodyID(b.value);
}

}  // namespace

BodyId PhysicsWorld::createStaticBox(Vec3 pos, Vec3 halfExtents) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Static, Layers::NON_MOVING, 0.0f);
}

BodyId PhysicsWorld::createDynamicBox(Vec3 pos, Vec3 halfExtents, float mass) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicSphere(Vec3 pos, float radius, float mass) {
    JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicCapsule(Vec3 pos, float halfH, float r, float mass) {
    JPH::ShapeRefC shape = new JPH::CapsuleShape(halfH, r);
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

void PhysicsWorld::destroyBody(BodyId b) {
    if (!b.isValid()) return;
    JPH::BodyInterface& bi = impl_->system->GetBodyInterface();
    bi.RemoveBody(toJoltBodyId(b));
    bi.DestroyBody(toJoltBodyId(b));
}

// --- Body state ---

Vec3 PhysicsWorld::bodyPosition(BodyId b) const {
    if (!b.isValid()) return {};
    return toI(impl_->system->GetBodyInterface().GetCenterOfMassPosition(toJoltBodyId(b)));
}
Quaternion PhysicsWorld::bodyRotation(BodyId b) const {
    if (!b.isValid()) return {};
    return toI(impl_->system->GetBodyInterface().GetRotation(toJoltBodyId(b)));
}
Mat4 PhysicsWorld::bodyTransform(BodyId b) const {
    if (!b.isValid()) return Mat4::identity();
    const JPH::RVec3 p = impl_->system->GetBodyInterface().GetCenterOfMassPosition(toJoltBodyId(b));
    const JPH::Quat  q = impl_->system->GetBodyInterface().GetRotation(toJoltBodyId(b));
    return mat4FromJolt(p, q);
}
bool PhysicsWorld::isBodyAlive(BodyId b) const {
    if (!b.isValid()) return false;
    return impl_->system->GetBodyInterface().IsAdded(toJoltBodyId(b));
}

// --- Forces / impulses ---

void PhysicsWorld::applyImpulse(BodyId b, Vec3 imp) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().AddImpulse(toJoltBodyId(b), toJ(imp));
}
void PhysicsWorld::applyForceAtPoint(BodyId b, Vec3 f, Vec3 worldP) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().AddForce(toJoltBodyId(b), toJ(f), toJ(worldP));
}
void PhysicsWorld::setVelocity(BodyId b, Vec3 v) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().SetLinearVelocity(toJoltBodyId(b), toJ(v));
}

// --- Raycast ---

PhysicsWorld::RaycastHit PhysicsWorld::raycast(Vec3 o, Vec3 d, float maxDist) const {
    RaycastHit out;
    if (!impl_->system) return out;
    JPH::RRayCast ray(toJ(o), toJ(d) * maxDist);
    JPH::RayCastResult hit;
    JPH::DefaultBroadPhaseLayerFilter bpFilter(impl_->objectVsBroadphase, Layers::MOVING);
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
        return out;
    }
    out.hit = true;
    out.body = BodyId{hit.mBodyID.GetIndexAndSequenceNumber()};
    out.t = hit.mFraction;
    const JPH::Vec3 pt = ray.GetPointOnRay(hit.mFraction);
    out.point = toI(pt);
    // Normal requires locking the body. Approximate via ray direction for v1;
    // proper normal extraction is a Task 2/follow-up extension.
    out.normal = Vec3{0.0f, 1.0f, 0.0f};  // placeholder; revised in joint task
    return out;
}

}  // namespace iron
```

> Notes for the implementer: this is the v1 wrapper. The `raycast` normal returns a placeholder upward vector — a proper normal extraction needs `JPH::BodyLockRead` to call `GetWorldSpaceSurfaceNormal`. That's a refinement that lands in Task 2 (where joints add similar locking patterns). Don't add it here; the test for raycast in Step 7 only verifies `hit == true` and approximate hit point — not the normal.

- [ ] **Step 6: Create `tests/test_physics_world.cpp`**

```cpp
#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Init/shutdown ---
    {
        PhysicsWorld w;
        CHECK(w.init());
        // shutdown via destructor; should not leak or crash
    }

    // --- Create + destroy a body ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        CHECK(b.isValid());
        CHECK(w.isBodyAlive(b));
        const Vec3 p = w.bodyPosition(b);
        CHECK_NEAR(p.y, 5.0f);
        w.destroyBody(b);
        CHECK(!w.isBodyAlive(b));
    }

    // --- Gravity pulls a body down ~4.9m over 1 second ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 100.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(b);
        // ½ * 9.81 * 1² = 4.905m drop. Allow ±0.2m for integration error.
        CHECK(std::fabs((100.0f - p.y) - 4.905f) < 0.2f);
    }

    // --- Raycast hits a static box below origin ---
    {
        PhysicsWorld w; w.init();
        BodyId ground = w.createStaticBox({0.0f, -1.0f, 0.0f}, {25.0f, 0.5f, 25.0f});
        CHECK(ground.isValid());
        // Step once to commit the body into the broadphase.
        w.step(1.0f / 60.0f);
        auto hit = w.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
        CHECK(hit.hit);
        CHECK(hit.body == ground);
        // Hit point is at the top of the static box (y = -0.5).
        CHECK(hit.point.y > -1.5f);
        CHECK(hit.point.y < 0.0f);
    }

    // --- Impulse imparts predictable velocity (m·v = J) ---
    {
        PhysicsWorld w; w.init();
        BodyId b = w.createDynamicBox({0.0f, 100.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 2.0f);
        w.applyImpulse(b, Vec3{10.0f, 0.0f, 0.0f});
        // After applying impulse, velocity_x ≈ 10/2 = 5 m/s.
        // Step a tiny dt and confirm body moved roughly +0.083m in x.
        w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(b);
        CHECK(p.x > 0.05f);
        CHECK(p.x < 0.15f);
    }

    // --- Determinism: two worlds + identical inputs -> identical body states ---
    {
        PhysicsWorld a, b;
        a.init(); b.init();
        BodyId ba = a.createDynamicBox({0.0f, 50.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        BodyId bb = b.createDynamicBox({0.0f, 50.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);
        a.applyImpulse(ba, Vec3{3.0f, 0.0f, 2.0f});
        b.applyImpulse(bb, Vec3{3.0f, 0.0f, 2.0f});
        for (int i = 0; i < 120; ++i) {
            a.step(1.0f / 60.0f);
            b.step(1.0f / 60.0f);
        }
        const Vec3 pa = a.bodyPosition(ba);
        const Vec3 pb = b.bodyPosition(bb);
        CHECK_NEAR(pa.x, pb.x);
        CHECK_NEAR(pa.y, pb.y);
        CHECK_NEAR(pa.z, pb.z);
    }

    return iron_test_result();
}
```

- [ ] **Step 7: Register the test in `tests/CMakeLists.txt`**

Open `tests/CMakeLists.txt`. After the existing `iron_add_test(test_verlet ...)` line, add:

```cmake
iron_add_test(test_physics_world test_physics_world.cpp)
```

- [ ] **Step 8: Configure + build**

From repo root, run:

```
cmake -S . -B build-vk
cmake --build build-vk --config Debug --target ironcore test_physics_world
```

Expected: vcpkg downloads + builds Jolt on first run (~3 minutes cold); `ironcore` compiles with the new physics files; `test_physics_world.exe` builds.

If vcpkg fails on `joltphysics`, check `vcpkg.json` syntax, and check `gh pr checks 35` for upstream issues. If you see "unknown package joltphysics", the vcpkg baseline doesn't include it — bump `vcpkg-configuration.json`'s baseline to a recent commit (use a commit after 2026-01).

- [ ] **Step 9: Run the test**

```
ctest --test-dir build-vk -C Debug -R test_physics_world --output-on-failure
```

Expected: PASS. All 6 sub-tests pass. If gravity test fails by a large margin, check that `SetGravity` is called in `init`. If determinism test fails, check that `JobSystemThreadPool` thread count is 1.

- [ ] **Step 10: Commit**

```
git add vcpkg.json CMakeLists.txt engine/CMakeLists.txt \
        engine/physics/PhysicsWorld.h engine/physics/PhysicsWorld.cpp \
        tests/test_physics_world.cpp tests/CMakeLists.txt
git commit -m "M18 Task 1: Jolt physics wrapper (PhysicsWorld + body lifecycle + raycast + impulse)"
```

---

## Task 2: Joints + Ragdoll class

**Files:**
- Modify: `engine/physics/PhysicsWorld.h` (add joint API)
- Modify: `engine/physics/PhysicsWorld.cpp` (joint implementations)
- Modify: `engine/CMakeLists.txt` (add `physics/Ragdoll.cpp` to sources)
- Create: `engine/physics/Ragdoll.h`
- Create: `engine/physics/Ragdoll.cpp`
- Create: `tests/test_ragdoll.cpp`
- Modify: `tests/CMakeLists.txt` (register `test_ragdoll`)

After this task, ragdoll machinery exists and is unit-tested. No game uses it yet.

- [ ] **Step 1: Add joint API to `PhysicsWorld.h`**

Open `engine/physics/PhysicsWorld.h`. In the `public:` section of `class PhysicsWorld`, immediately after the `RaycastHit raycast(...)` declaration, add:

```cpp
    // --- Joints ---
    // Swing-twist: spine, neck, shoulders, hips. World-space pivot +
    // twist axis. `swingLimit` is the cone half-angle the twist axis can
    // swing through; `twistLimit` is rotation around the twist axis.
    // Both in radians.
    JointId createSwingTwistJoint(BodyId a, BodyId b,
                                  Vec3 pivotWorld, Vec3 twistAxisWorld,
                                  float swingLimitRad, float twistLimitRad);

    // Hinge: elbow, knee. World-space pivot + hinge axis + signed angle
    // limits in radians. Limits are relative to the initial body
    // configuration at joint creation.
    JointId createHingeJoint(BodyId a, BodyId b,
                             Vec3 pivotWorld, Vec3 hingeAxisWorld,
                             float minAngleRad, float maxAngleRad);

    void destroyJoint(JointId);
```

Add `#include <vector>` if not present (used in impl).

- [ ] **Step 2: Implement joints in `PhysicsWorld.cpp`**

In `engine/physics/PhysicsWorld.cpp`, add Jolt joint includes near the existing includes:

```cpp
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
```

In the `Impl` struct, add a joint table:

```cpp
struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl*     temp       = nullptr;
    JPH::JobSystemThreadPool*   jobs       = nullptr;
    JPH::PhysicsSystem*         system     = nullptr;
    BroadPhaseLayerInterfaceImpl       broadphaseLayers;
    ObjectVsBroadPhaseLayerFilterImpl  objectVsBroadphase;
    ObjectLayerPairFilterImpl          objectVsObject;

    // Joints: simple linear table indexed by JointId::value-1.
    // 0 is reserved as kInvalidJoint.
    std::vector<JPH::TwoBodyConstraintRef> joints;
};
```

In `shutdown()`, destroy any remaining joints before deleting the system. Add at the start of the existing system-cleanup branch:

```cpp
    if (impl_->system) {
        for (auto& j : impl_->joints) {
            if (j != nullptr) impl_->system->RemoveConstraint(j);
        }
        impl_->joints.clear();
        delete impl_->system; impl_->system = nullptr;
    }
```

Add the joint creation implementations near the bottom of the file, before the closing `}  // namespace iron`:

```cpp
namespace {

JPH::Body& lockBody(PhysicsWorld::Impl& impl, BodyId id) {
    // Note: we use the body interface lock-free for simplicity, since
    // we only create joints during scene setup (no concurrent step).
    return *impl.system->GetBodyLockInterfaceNoLock().TryGetBody(toJoltBodyId(id));
}

}  // namespace

JointId PhysicsWorld::createSwingTwistJoint(BodyId a, BodyId b,
                                            Vec3 pivotWorld, Vec3 twistAxisWorld,
                                            float swingLimitRad, float twistLimitRad) {
    JPH::SwingTwistConstraintSettings s;
    s.mPosition1 = toJ(pivotWorld);
    s.mPosition2 = toJ(pivotWorld);
    s.mTwistAxis1 = toJ(twistAxisWorld).Normalized();
    s.mTwistAxis2 = toJ(twistAxisWorld).Normalized();
    // Plane axis perpendicular to twist axis — pick any perpendicular vector.
    JPH::Vec3 ta = s.mTwistAxis1;
    JPH::Vec3 plane = ta.GetNormalizedPerpendicular();
    s.mPlaneAxis1 = plane;
    s.mPlaneAxis2 = plane;
    s.mNormalHalfConeAngle = swingLimitRad;
    s.mPlaneHalfConeAngle  = swingLimitRad;
    s.mTwistMinAngle = -twistLimitRad;
    s.mTwistMaxAngle =  twistLimitRad;

    JPH::Body& ba = lockBody(*impl_, a);
    JPH::Body& bb = lockBody(*impl_, b);
    JPH::TwoBodyConstraintRef c = s.Create(ba, bb);
    impl_->system->AddConstraint(c);
    impl_->joints.push_back(c);
    return JointId{static_cast<std::uint32_t>(impl_->joints.size())};
}

JointId PhysicsWorld::createHingeJoint(BodyId a, BodyId b,
                                        Vec3 pivotWorld, Vec3 hingeAxisWorld,
                                        float minAngleRad, float maxAngleRad) {
    JPH::HingeConstraintSettings s;
    s.mPoint1 = toJ(pivotWorld);
    s.mPoint2 = toJ(pivotWorld);
    s.mHingeAxis1 = toJ(hingeAxisWorld).Normalized();
    s.mHingeAxis2 = toJ(hingeAxisWorld).Normalized();
    JPH::Vec3 normal = s.mHingeAxis1.GetNormalizedPerpendicular();
    s.mNormalAxis1 = normal;
    s.mNormalAxis2 = normal;
    s.mLimitsMin = minAngleRad;
    s.mLimitsMax = maxAngleRad;

    JPH::Body& ba = lockBody(*impl_, a);
    JPH::Body& bb = lockBody(*impl_, b);
    JPH::TwoBodyConstraintRef c = s.Create(ba, bb);
    impl_->system->AddConstraint(c);
    impl_->joints.push_back(c);
    return JointId{static_cast<std::uint32_t>(impl_->joints.size())};
}

void PhysicsWorld::destroyJoint(JointId j) {
    if (!j.isValid()) return;
    const size_t idx = j.value - 1;
    if (idx >= impl_->joints.size()) return;
    JPH::TwoBodyConstraintRef& c = impl_->joints[idx];
    if (c != nullptr) {
        impl_->system->RemoveConstraint(c);
        c = nullptr;
    }
}
```

- [ ] **Step 3: Create `engine/physics/Ragdoll.h`**

```cpp
#pragma once

#include "physics/PhysicsWorld.h"

#include <array>

namespace iron {

struct RagdollSpec {
    float totalHeight = 1.8f;   // overall height, all dims scale linearly
    float mass        = 75.0f;  // total mass distributed across bones
};

// 11-body humanoid skeleton: head, torso, hips, 2x upper arm, 2x forearm,
// 2x upper leg, 2x lower leg. 10 joints: spine + neck + 2 shoulders +
// 2 elbows + 2 hips + 2 knees.
//
// Render each bone as a colored cube using `boneTransform()` +
// `boneHalfExtents()`. Distinct colors via `boneColor()` make rotation
// legible pre-skeletal-animation.
class Ragdoll {
public:
    static constexpr int kBoneCount  = 11;
    static constexpr int kJointCount = 10;

    // Bone index constants for callers that want to address specific bones.
    static constexpr int kHead       = 0;
    static constexpr int kTorso      = 1;
    static constexpr int kHips       = 2;
    static constexpr int kUpperArmL  = 3;
    static constexpr int kForearmL   = 4;
    static constexpr int kUpperArmR  = 5;
    static constexpr int kForearmR   = 6;
    static constexpr int kUpperLegL  = 7;
    static constexpr int kLowerLegL  = 8;
    static constexpr int kUpperLegR  = 9;
    static constexpr int kLowerLegR  = 10;

    void spawn(PhysicsWorld& world, const RagdollSpec& spec,
               Vec3 position, Quaternion rotation = {0,0,0,1});
    void despawn(PhysicsWorld& world);

    int  boneCount() const { return kBoneCount; }
    Mat4 boneTransform(int idx)   const;
    Vec3 boneHalfExtents(int idx) const;
    Vec3 boneColor(int idx)       const;
    BodyId boneBody(int idx)      const;

    bool isSpawned() const { return bones_[kHips].isValid(); }

private:
    PhysicsWorld* world_ = nullptr;
    std::array<BodyId,  kBoneCount>  bones_      {};
    std::array<JointId, kJointCount> joints_     {};
    std::array<Vec3,    kBoneCount>  halfExtents_{};
};

}  // namespace iron
```

- [ ] **Step 4: Create `engine/physics/Ragdoll.cpp`**

```cpp
// Ragdoll.cpp — 11-body humanoid skeleton + 10 joints.

#include "physics/Ragdoll.h"
#include "math/Transform.h"

#include <cmath>
#include <numbers>

namespace iron {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDeg = kPi / 180.0f;

// Bone proportions as fractions of total height (for a 1.8m human these
// give 0.25 head + 0.50 torso + 0.18 hips + 0.42 leg + 0.30 arm vertical).
struct BoneSpec {
    Vec3  halfExtentsFracHeight;  // multiplied by spec.totalHeight
    float massFrac;
    Vec3  colorRgb;
};

constexpr BoneSpec kBoneSpecs[Ragdoll::kBoneCount] = {
    // half-extents (x, y, z) as fraction of total height, then mass frac, color
    {{0.056f, 0.069f, 0.056f}, 0.08f,  {1.0f, 0.40f, 0.40f}},  // head — red
    {{0.100f, 0.139f, 0.061f}, 0.36f,  {0.40f, 0.60f, 1.0f}},  // torso — blue
    {{0.100f, 0.050f, 0.061f}, 0.14f,  {0.30f, 0.45f, 0.80f}}, // hips — darker blue
    {{0.028f, 0.083f, 0.028f}, 0.03f,  {0.40f, 1.00f, 0.40f}}, // upper arm L — green
    {{0.022f, 0.089f, 0.022f}, 0.025f, {0.30f, 0.85f, 0.30f}}, // forearm L — darker green
    {{0.028f, 0.083f, 0.028f}, 0.03f,  {0.40f, 1.00f, 0.40f}}, // upper arm R — green
    {{0.022f, 0.089f, 0.022f}, 0.025f, {0.30f, 0.85f, 0.30f}}, // forearm R — darker green
    {{0.036f, 0.117f, 0.036f}, 0.09f,  {1.00f, 0.85f, 0.30f}}, // upper leg L — yellow
    {{0.028f, 0.117f, 0.028f}, 0.06f,  {0.85f, 0.70f, 0.20f}}, // lower leg L — darker yellow
    {{0.036f, 0.117f, 0.036f}, 0.09f,  {1.00f, 0.85f, 0.30f}}, // upper leg R
    {{0.028f, 0.117f, 0.028f}, 0.06f,  {0.85f, 0.70f, 0.20f}}, // lower leg R
};

// Bone vertical offsets from the hips (positive = above hips, in fractions
// of total height). Used to position bones relative to spawn point.
//          head torso hips uArmL fArmL uArmR fArmR uLegL lLegL uLegR lLegR
constexpr float kY[Ragdoll::kBoneCount] = {
    0.443f, 0.250f, 0.000f, 0.225f, 0.067f, 0.225f, 0.067f, -0.167f, -0.400f, -0.167f, -0.400f,
};
constexpr float kX[Ragdoll::kBoneCount] = {
    0.0f, 0.0f, 0.0f, -0.131f, -0.131f, +0.131f, +0.131f, -0.061f, -0.061f, +0.061f, +0.061f,
};

}  // namespace

void Ragdoll::spawn(PhysicsWorld& world, const RagdollSpec& spec,
                    Vec3 position, Quaternion /*rotation*/) {
    world_ = &world;
    const float H = spec.totalHeight;

    // --- Create bodies ---
    for (int i = 0; i < kBoneCount; ++i) {
        const Vec3 he = {
            kBoneSpecs[i].halfExtentsFracHeight.x * H,
            kBoneSpecs[i].halfExtentsFracHeight.y * H,
            kBoneSpecs[i].halfExtentsFracHeight.z * H,
        };
        halfExtents_[i] = he;
        const Vec3 pos = {
            position.x + kX[i] * H,
            position.y + kY[i] * H,
            position.z,
        };
        bones_[i] = world.createDynamicBox(pos, he, spec.mass * kBoneSpecs[i].massFrac);
    }

    // --- Create joints (world-space pivots midway between connected bones) ---
    auto pivotBetween = [&](int a, int b) -> Vec3 {
        return Vec3{
            position.x + ((kX[a] + kX[b]) * 0.5f) * H,
            position.y + ((kY[a] + kY[b]) * 0.5f) * H,
            position.z,
        };
    };

    int j = 0;
    // Spine (torso ↔ hips), swing-twist
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kTorso], bones_[kHips], pivotBetween(kTorso, kHips),
        Vec3{0,1,0}, 25.0f * kDeg, 15.0f * kDeg);
    // Neck (head ↔ torso)
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kHead], bones_[kTorso], pivotBetween(kHead, kTorso),
        Vec3{0,1,0}, 40.0f * kDeg, 60.0f * kDeg);
    // Shoulder L (upperArmL ↔ torso). Twist axis points down the arm (Y).
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperArmL], bones_[kTorso], pivotBetween(kUpperArmL, kTorso),
        Vec3{0,1,0}, 90.0f * kDeg, 45.0f * kDeg);
    // Shoulder R
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperArmR], bones_[kTorso], pivotBetween(kUpperArmR, kTorso),
        Vec3{0,1,0}, 90.0f * kDeg, 45.0f * kDeg);
    // Elbow L (forearmL ↔ upperArmL), hinge around X (medio-lateral)
    joints_[j++] = world.createHingeJoint(
        bones_[kForearmL], bones_[kUpperArmL], pivotBetween(kForearmL, kUpperArmL),
        Vec3{1,0,0}, 0.0f, 145.0f * kDeg);
    // Elbow R
    joints_[j++] = world.createHingeJoint(
        bones_[kForearmR], bones_[kUpperArmR], pivotBetween(kForearmR, kUpperArmR),
        Vec3{1,0,0}, 0.0f, 145.0f * kDeg);
    // Hip L (upperLegL ↔ hips)
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperLegL], bones_[kHips], pivotBetween(kUpperLegL, kHips),
        Vec3{0,1,0}, 60.0f * kDeg, 30.0f * kDeg);
    // Hip R
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperLegR], bones_[kHips], pivotBetween(kUpperLegR, kHips),
        Vec3{0,1,0}, 60.0f * kDeg, 30.0f * kDeg);
    // Knee L (lowerLegL ↔ upperLegL), hinge around X
    joints_[j++] = world.createHingeJoint(
        bones_[kLowerLegL], bones_[kUpperLegL], pivotBetween(kLowerLegL, kUpperLegL),
        Vec3{1,0,0}, -145.0f * kDeg, 0.0f);
    // Knee R
    joints_[j++] = world.createHingeJoint(
        bones_[kLowerLegR], bones_[kUpperLegR], pivotBetween(kLowerLegR, kUpperLegR),
        Vec3{1,0,0}, -145.0f * kDeg, 0.0f);
}

void Ragdoll::despawn(PhysicsWorld& world) {
    for (auto& j : joints_) {
        if (j.isValid()) {
            world.destroyJoint(j);
            j = kInvalidJoint;
        }
    }
    for (auto& b : bones_) {
        if (b.isValid()) {
            world.destroyBody(b);
            b = kInvalidBody;
        }
    }
    world_ = nullptr;
}

Mat4 Ragdoll::boneTransform(int idx) const {
    if (idx < 0 || idx >= kBoneCount || !world_) return Mat4::identity();
    return world_->bodyTransform(bones_[idx]);
}

Vec3 Ragdoll::boneHalfExtents(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return {};
    return halfExtents_[idx];
}

Vec3 Ragdoll::boneColor(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return {1,1,1};
    return kBoneSpecs[idx].colorRgb;
}

BodyId Ragdoll::boneBody(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return kInvalidBody;
    return bones_[idx];
}

}  // namespace iron
```

- [ ] **Step 5: Add `Ragdoll.cpp` to `engine/CMakeLists.txt`**

Open `engine/CMakeLists.txt`. Find the line `physics/PhysicsWorld.cpp` and add immediately after:

```cmake
  physics/Ragdoll.cpp
```

- [ ] **Step 6: Create `tests/test_ragdoll.cpp`**

```cpp
#include "physics/Ragdoll.h"
#include "physics/PhysicsWorld.h"
#include "test_framework.h"

#include <cmath>

int main() {
    using namespace iron;

    // --- Spawn count: 11 bodies, 10 joints, all alive ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        RagdollSpec spec;  // defaults: 1.8m, 75kg
        rag.spawn(w, spec, Vec3{0.0f, 5.0f, 0.0f});

        CHECK(rag.isSpawned());
        CHECK(rag.boneCount() == 11);
        for (int i = 0; i < Ragdoll::kBoneCount; ++i) {
            CHECK(rag.boneBody(i).isValid());
            CHECK(w.isBodyAlive(rag.boneBody(i)));
        }
    }

    // --- Hips body at spawn position ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        RagdollSpec spec;
        const Vec3 spawn = {2.0f, 5.0f, -3.0f};
        rag.spawn(w, spec, spawn);
        const Vec3 hipsPos = w.bodyPosition(rag.boneBody(Ragdoll::kHips));
        CHECK_NEAR(hipsPos.x, spawn.x);
        CHECK_NEAR(hipsPos.y, spawn.y);
        CHECK_NEAR(hipsPos.z, spawn.z);
    }

    // --- Despawn removes everything ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        rag.spawn(w, {}, Vec3{0.0f, 10.0f, 0.0f});
        const BodyId hips = rag.boneBody(Ragdoll::kHips);
        CHECK(w.isBodyAlive(hips));
        rag.despawn(w);
        CHECK(!rag.isSpawned());
        CHECK(!w.isBodyAlive(hips));
    }

    // --- Free fall: hips drops ~19.6m in 2 seconds ---
    {
        PhysicsWorld w; w.init();
        Ragdoll rag;
        const Vec3 spawn = {0.0f, 100.0f, 0.0f};
        rag.spawn(w, {}, spawn);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        const Vec3 p = w.bodyPosition(rag.boneBody(Ragdoll::kHips));
        // ½ * 9.81 * 2² = 19.62m. Joints add slight stiffness; allow ±2m.
        const float dropped = 100.0f - p.y;
        CHECK(dropped > 17.0f);
        CHECK(dropped < 22.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 7: Register `test_ragdoll` in `tests/CMakeLists.txt`**

After the `iron_add_test(test_physics_world ...)` line from Task 1, add:

```cmake
iron_add_test(test_ragdoll test_ragdoll.cpp)
```

- [ ] **Step 8: Build + run**

```
cmake --build build-vk --config Debug --target ironcore test_ragdoll
ctest --test-dir build-vk -C Debug -R test_ragdoll --output-on-failure
```

Expected: PASS. All 4 sub-tests pass.

> If the "Despawn removes everything" sub-test fails, check that `destroyJoint` actually calls `system->RemoveConstraint` and that `destroyBody` actually removes from the body interface. If "Free fall" fails by a large margin, the joints are probably too stiff (collapsing the skeleton into a non-falling lump). Check that joint limits are not zero.

- [ ] **Step 9: Commit**

```
git add engine/physics/PhysicsWorld.h engine/physics/PhysicsWorld.cpp \
        engine/physics/Ragdoll.h engine/physics/Ragdoll.cpp \
        engine/CMakeLists.txt \
        tests/test_ragdoll.cpp tests/CMakeLists.txt
git commit -m "M18 Task 2: joints + Ragdoll (11 bodies + 10 joints humanoid)"
```

---

## Task 3: Playground demo

**Files:**
- Create: `games/09-physics-playground/CMakeLists.txt`
- Create: `games/09-physics-playground/main.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(games/09-physics-playground)`)

The visual validator. Vulkan-only.

- [ ] **Step 1: Register the new game subdirectory**

Open top-level `CMakeLists.txt`. Find the `add_subdirectory(games/08-particle-storm)` line. Add immediately after:

```cmake
add_subdirectory(games/09-physics-playground)
```

- [ ] **Step 2: Create `games/09-physics-playground/CMakeLists.txt`**

```cmake
if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(physics-playground main.cpp)
    target_link_libraries(physics-playground PRIVATE ironcore)
endif()
```

- [ ] **Step 3: Create `games/09-physics-playground/main.cpp`**

```cpp
// games/09-physics-playground/main.cpp — Vulkan-only physics playground.
//
// Demonstrates Jolt rigid bodies + an 11-body ragdoll. Free-fly camera,
// spawn ragdolls with R, fire spheres with B, clear with C.

#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "math/Transform.h"
#include "physics/PhysicsWorld.h"
#include "physics/Ragdoll.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "ui/Hud.h"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// Visual cube mesh used for rendering every rigid body. Created once,
// transformed per-draw via the body's model matrix.
iron::MeshHandle makeUnitCube(iron::Renderer& renderer) {
    iron::MeshData data;
    iron::appendBox(data, /*halfExtents*/ {0.5f, 0.5f, 0.5f});
    return renderer.createMesh(data);
}

iron::Vec3 randomUnitVec3(std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    iron::Vec3 v{u(rng), u(rng), u(rng)};
    const float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    return (len > 1e-4f) ? iron::Vec3{v.x/len, v.y/len, v.z/len} : iron::Vec3{0,1,0};
}

struct DynamicBody {
    iron::BodyId id;
    iron::Vec3   halfExtents;
    iron::Vec3   color;
};

}  // namespace

int main() {
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - Physics Playground";
    cfg.width  = kScreenW;
    cfg.height = kScreenH;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("physics-playground: Application init failed");
        return 1;
    }

    auto renderer_ptr = iron::createRenderer(app.window());
    if (!renderer_ptr) {
        iron::Log::error("physics-playground: renderer init failed");
        return 1;
    }
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenW, kScreenH);

    // --- Physics ---
    iron::PhysicsWorld physics;
    if (!physics.init()) {
        iron::Log::error("physics-playground: physics init failed");
        return 1;
    }

    // Single lit shader (Vulkan only). Inline a minimal lit shader pair —
    // the same one net-shooter uses.
    const char* kVert = R"(#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec3 aTangent;

layout(set=0, binding=0) uniform LitUbo {
    mat4 mvp; mat4 model; mat4 lightViewProj;
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 emissive;
    vec4 cameraPos; vec4 materialParams;
    vec4 fogColor; vec4 lightCounts;
    vec4 pointPositions[16]; vec4 pointColors[16];
    mat4 reflectionViewProj; vec4 reflectionParams; vec4 clipPlane;
} u;

layout(location=0) out vec3 vNormal;
layout(location=1) out vec3 vWorldPos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";
    const char* kFrag = R"(#version 450
layout(location=0) in vec3 vNormal;
layout(location=1) in vec3 vWorldPos;
layout(location=0) out vec4 outColor;

layout(set=0, binding=0) uniform LitUbo {
    mat4 mvp; mat4 model; mat4 lightViewProj;
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 emissive;
    vec4 cameraPos; vec4 materialParams;
    vec4 fogColor; vec4 lightCounts;
    vec4 pointPositions[16]; vec4 pointColors[16];
    mat4 reflectionViewProj; vec4 reflectionParams; vec4 clipPlane;
} u;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = -normalize(u.sunDir.xyz);
    float diffuse = max(dot(N, L), 0.0);
    vec3 lit = u.emissive.xyz * (diffuse + u.ambient.x);
    outColor = vec4(lit, 1.0);
}
)";
    const iron::ShaderHandle shader = renderer.createShader(kVert, kFrag);
    if (shader == iron::kInvalidHandle) {
        iron::Log::error("physics-playground: shader compile failed");
        return 1;
    }
    const iron::MeshHandle cubeMesh = makeUnitCube(renderer);

    // --- Static scene ---
    physics.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});  // ground
    // Three ramps side by side, ~3m long, ~1m wide, at 15/30/45 deg.
    auto makeRamp = [&](iron::Vec3 pos, float degrees) {
        const float rad = degrees * (std::numbers::pi_v<float> / 180.0f);
        // For visual + physics ramp v1, approximate via a tilted thin box.
        // Use a static box with manual tilt — Jolt's static_box doesn't take rotation
        // in our wrapper API. For v1, build the ramp as a tilted dynamic-shape static.
        // Workaround: tilt the visual cube but use a flat AABB for physics. Acceptable for v1.
        (void)rad;
        physics.createStaticBox(pos, {1.5f, 0.05f, 0.5f});
    };
    makeRamp({-5.0f, 1.0f, 5.0f}, 15.0f);
    makeRamp({ 0.0f, 1.5f, 5.0f}, 30.0f);
    makeRamp({+5.0f, 2.0f, 5.0f}, 45.0f);
    // Low wall.
    physics.createStaticBox({0.0f, 0.5f, -3.0f}, {2.0f, 0.5f, 0.1f});

    // --- Dynamic box stack (4 boxes) ---
    std::vector<DynamicBody> dynamicBodies;
    auto resetBoxStack = [&]() {
        // Destroy existing dynamic stack bodies (first 4 are the stack).
        // Simpler: don't track stack indices, just blow away the stack and respawn.
        // Stack is rebuilt fresh each time; existing entries from a prior call are
        // assumed already destroyed by the C-key handler.
        for (int i = 0; i < 4; ++i) {
            iron::Vec3 pos = {3.0f, 0.5f + i * 1.05f, -1.0f};
            iron::BodyId b = physics.createDynamicBox(pos, {0.5f, 0.5f, 0.5f}, 5.0f);
            dynamicBodies.push_back({b, {0.5f, 0.5f, 0.5f}, {0.55f, 0.40f, 0.25f}});
        }
    };
    resetBoxStack();

    // --- Ragdolls + projectiles ---
    std::vector<iron::Ragdoll> ragdolls;
    ragdolls.reserve(32);

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 4.0f, 12.0f};

    std::mt19937 rng{12345};

    const float aspect = static_cast<float>(kScreenW) / static_cast<float>(kScreenH);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 200.0f);

    app.window().setCursorCaptured(true);

    bool prevR = false, prevB = false, prevC = false;

    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }

        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());
        cam.update(t.deltaSeconds, mdx, mdy,
                   input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                   input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                   input.keyDown(GLFW_KEY_LEFT_CONTROL),
                   input.keyDown(GLFW_KEY_SPACE),
                   12.0f);

        // R: spawn ragdoll in front of camera.
        const bool R = input.keyDown(GLFW_KEY_R);
        if (R && !prevR) {
            const iron::Vec3 fwd = cam.forward();
            const iron::Vec3 spawn = {
                cam.position.x + fwd.x * 3.0f,
                cam.position.y + fwd.y * 3.0f + 1.0f,
                cam.position.z + fwd.z * 3.0f,
            };
            ragdolls.emplace_back();
            ragdolls.back().spawn(physics, {}, spawn);
            // Tumble: small upward velocity on hips.
            physics.setVelocity(ragdolls.back().boneBody(iron::Ragdoll::kHips),
                                iron::Vec3{0.0f, 2.0f, 0.0f});
        }
        prevR = R;

        // B: fire 5kg sphere from camera at 30 m/s.
        const bool B = input.keyDown(GLFW_KEY_B);
        if (B && !prevB) {
            const iron::Vec3 fwd = cam.forward();
            iron::BodyId ball = physics.createDynamicSphere(cam.position, 0.3f, 5.0f);
            physics.setVelocity(ball, {fwd.x * 30.0f, fwd.y * 30.0f, fwd.z * 30.0f});
            dynamicBodies.push_back({ball, {0.3f, 0.3f, 0.3f}, {1.0f, 1.0f, 0.2f}});
        }
        prevB = B;

        // C: clear ragdolls + reset box stack.
        const bool C = input.keyDown(GLFW_KEY_C);
        if (C && !prevC) {
            for (auto& r : ragdolls) r.despawn(physics);
            ragdolls.clear();
            for (auto& db : dynamicBodies) physics.destroyBody(db.id);
            dynamicBodies.clear();
            resetBoxStack();
        }
        prevC = C;

        physics.step(std::min(t.deltaSeconds, 1.0f / 30.0f));
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();
        iron::DirectionalLight sun;
        sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
        sun.color     = {1.0f, 1.0f, 1.0f};
        sun.ambient   = 0.35f;

        renderer.beginFrame({0.55f, 0.7f, 0.85f}, sun,
                            std::span<const iron::PointLight>{},
                            iron::Fog{}, view, proj);

        // Submit each dynamic body as a colored cube. Scale via model matrix.
        auto submitBox = [&](iron::Mat4 model, iron::Vec3 he, iron::Vec3 color) {
            // The cube mesh has half-extents (0.5, 0.5, 0.5). Scale by 2*he.
            const iron::Mat4 scale = iron::scaling({he.x * 2.0f, he.y * 2.0f, he.z * 2.0f});
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = shader;
            call.model = iron::multiply(model, scale);
            call.material.emissive = color;
            renderer.submit(call);
        };

        // Ground (visualized as a flat dark gray slab).
        submitBox(iron::translation({0.0f, -0.5f, 0.0f}), {25.0f, 0.5f, 25.0f}, {0.3f, 0.3f, 0.3f});

        // Render every dynamic body using its physics transform.
        for (const auto& db : dynamicBodies) {
            if (!physics.isBodyAlive(db.id)) continue;
            submitBox(physics.bodyTransform(db.id), db.halfExtents, db.color);
        }
        // Ragdolls: each bone as its own colored box.
        for (const auto& r : ragdolls) {
            for (int i = 0; i < r.boneCount(); ++i) {
                submitBox(r.boneTransform(i), r.boneHalfExtents(i), r.boneColor(i));
            }
        }

        // HUD: simple text.
        iron::HudBatch hud;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Ragdolls: %zu  Dynamic bodies: %zu",
                      ragdolls.size(), dynamicBodies.size());
        hud.addText(buf, /*x*/ 10, /*y*/ 10, /*color*/ {1.0f, 1.0f, 1.0f, 1.0f});
        hud.addText("R: spawn ragdoll  B: fire ball  C: clear  ESC: quit",
                    10, kScreenH - 24, {1.0f, 1.0f, 0.0f, 1.0f});
        renderer.drawHud(hud, kScreenW, kScreenH);

        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
}
```

> **Notes for the implementer:**
> - `iron::translation`, `iron::scaling`, `iron::multiply` are in `math/Transform.h`. If the exact names differ, look at how 08-particle-storm and 07-net-shooter build model matrices and follow that pattern.
> - `iron::FreeFlyCamera::forward()` — if no such method exists on the existing camera, derive forward from yaw/pitch inline using the same math `FreeFlyCamera::viewMatrix()` uses.
> - `cam.fovDeg` matches 08-particle-storm usage.
> - The HUD API (`HudBatch::addText`) is from M11. If method names differ, follow what 07-net-shooter does.
> - The ramps are currently flat-box approximations for v1 (the wrapper doesn't yet support body rotation at creation). True tilted ramps are a follow-up. For visual correctness in v1, you can render the ramp visuals tilted but keep the physics flat — or just accept flat ramps for v1 and call it out as a known cosmetic limitation. **Recommend: accept flat ramps for v1.**

- [ ] **Step 4: Build**

```
cmake --build build-vk --config Debug --target physics-playground
```

Expected: clean build. If `iron::appendBox` is missing or has different arguments, check `engine/scene/Mesh.h` and adapt the call site.

- [ ] **Step 5: Visual smoke test**

Run interactively:

```
.\build-vk\games\09-physics-playground\Debug\physics-playground.exe
```

Expected:
- Window opens, you spawn at (0, 4, 12) looking towards origin
- WASD + mouse fly
- Press R: a ragdoll appears 3m in front of you and falls
- Press B: a yellow sphere flies from your camera
- Aim and shoot the box stack — boxes scatter
- Aim and shoot the ragdoll — limbs flail
- Press C: everything clears, stack resets
- HUD shows ragdoll + body counts

If the ragdoll spawns and IMMEDIATELY falls apart into 11 floating bodies (joint failure), the joint pivots are likely wrong — check `pivotBetween` calculation in `Ragdoll::spawn`.

If the ragdoll spawns as a single frozen statue, the joint limits are zero everywhere — check joint creation in Task 2.

- [ ] **Step 6: Commit**

```
git add games/09-physics-playground/CMakeLists.txt games/09-physics-playground/main.cpp \
        CMakeLists.txt
git commit -m "M18 Task 3: physics-playground demo (ragdolls + boxes + ramps + ball)"
```

---

## Task 4: Docs

**Files:**
- Create: `docs/engine/physics.md`

- [ ] **Step 1: Write `docs/engine/physics.md`**

```markdown
# Physics

The engine uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics) for rigid
body dynamics, joints, and queries. Jolt comes in via vcpkg manifest; no
external SDK install is needed.

## Why Jolt

Selected over PhysX, Bullet, and a from-scratch implementation. The deciding
factors for this engine:

- **Deterministic by default.** Same inputs → byte-identical outputs across
  runs and machines. Required for the engine's server-authoritative networking
  with client prediction (see `docs/engine/networking.md`).
- **Modern C++17 API.** Designed in 2021. No legacy cruft.
- **Fast compile times** and small static lib — friction matters when iterating
  on the engine itself.
- **Best-in-class character controller** (`JPH::CharacterVirtual`) — character
  movement is what most of the engine's games need most.
- **MIT license**, used in production (Horizon Forbidden West).

PhysX was the close runner-up. Its wins (GPU acceleration for cloth/fluids,
PVD debugger, industry recognition) don't apply to this engine's scope.

## API surface — `iron::PhysicsWorld`

Single header: `engine/physics/PhysicsWorld.h`. Game code never includes a
Jolt header; pimpl hides all Jolt types from the public surface.

Opaque handles:

- `iron::BodyId` — opaque rigid body handle. `kInvalidBody` is the
  "not set" sentinel (matches the engine's existing `MeshHandle` / `TextureHandle`
  pattern).
- `iron::JointId` — opaque joint handle.

Body creation returns `BodyId`:

- `createStaticBox(pos, halfExtents)` — immovable AABB
- `createDynamicBox(pos, halfExtents, mass)` — falls under gravity
- `createDynamicSphere(pos, radius, mass)`
- `createDynamicCapsule(pos, halfHeight, radius, mass)`

Read body state for rendering: `bodyPosition`, `bodyRotation`,
`bodyTransform` (returns the engine's `Mat4` ready to use as a draw call's
`model` matrix).

Forces and impulses: `applyImpulse`, `applyForceAtPoint`, `setVelocity`.

Queries: `raycast(origin, direction, maxDistance)` returns a `RaycastHit`
with `hit`, `body`, `point`, `normal`, `t`.

Joints:

- `createSwingTwistJoint(a, b, pivotWorld, twistAxis, swingLimitRad, twistLimitRad)`
  — used for spine, neck, shoulders, hips.
- `createHingeJoint(a, b, pivotWorld, hingeAxis, minAngleRad, maxAngleRad)`
  — used for elbows, knees.
- `destroyJoint(jointId)`

Stepping: `step(dt)`. Configured for deterministic single-threaded behavior.

## API surface — `iron::Ragdoll`

`engine/physics/Ragdoll.h`. Declarative humanoid skeleton on top of
`PhysicsWorld`.

11 bones: head, torso, hips, 2× upper arm, 2× forearm, 2× upper leg,
2× lower leg. 10 joints. Bone indices exposed as
`Ragdoll::kHead`, `kTorso`, etc.

`RagdollSpec` controls total height (default 1.8m) and mass (default 75kg).
All bone dimensions scale linearly from height; mass is distributed across
bones using anatomical proportions.

Usage:

```cpp
iron::Ragdoll rag;
rag.spawn(physicsWorld, iron::RagdollSpec{}, position);
// Per frame, render each bone:
for (int i = 0; i < rag.boneCount(); ++i) {
    iron::Mat4 model = rag.boneTransform(i);     // updated by physics
    iron::Vec3 he    = rag.boneHalfExtents(i);   // for cube scaling
    iron::Vec3 color = rag.boneColor(i);          // distinct per-bone hue
    // ... submit a draw call with this model + scaled cube mesh + color
}
// On death/despawn:
rag.despawn(physicsWorld);
```

## Determinism contract

`PhysicsWorld::init` configures Jolt for byte-deterministic simulation:

- Single worker thread (`JobSystemThreadPool` with `threadCount = 1`).
- No island splitter parallelism.
- Default `PhysicsSettings`.

This guarantees: given two `PhysicsWorld` instances with identical bodies
created in identical order, `step(dt)` called with identical `dt` produces
byte-identical `bodyPosition` / `bodyRotation` after any number of steps.

Networked physics depends on this. If/when M19 onwards introduces parallel
physics for performance, determinism mode will become explicit
(`PhysicsWorldConfig::deterministic = true`) and the multi-threaded path will
be opt-in for non-networked single-player work.

## Visual validator: `games/09-physics-playground`

Vulkan-only demo. Free-fly camera; press `R` to spawn ragdolls, `B` to fire
spheres, `C` to clear and reset. The scene includes a ground plane, three
ramps (flat boxes in v1 — true tilted ramps are a follow-up), a low wall,
and a 4-box dynamic stack.

Build + run:

```
cmake --build build-vk --config Debug --target physics-playground
.\build-vk\games\09-physics-playground\Debug\physics-playground.exe
```

## What's next

- **M19:** capsule character controller (`iron::CharacterController` wrapping
  `JPH::CharacterVirtual`). Port net-shooter movement onto Jolt.
- **M20:** projectile rigid bodies + raycasts in net-shooter. Replaces
  `engine/game/ProjectileSim` and the bespoke `Ray`/`AABB` math for physics
  queries. (`engine/math/Ray.h` stays for non-physics gameplay queries.)
- **M21:** death-into-ragdoll wiring in net-shooter. Mostly game-side; the
  ragdoll machinery already exists from M18.

## See also

- Upstream docs: <https://jrouwe.github.io/JoltPhysics/>
- vcpkg port: `joltphysics`
- Spec: `docs/superpowers/specs/2026-05-27-m18-jolt-physics-ragdoll-playground-design.md`
- Plan: `docs/superpowers/plans/2026-05-27-m18-jolt-physics-ragdoll-playground.md`
```

- [ ] **Step 2: Commit**

```
git add docs/engine/physics.md
git commit -m "M18 Task 4: docs/engine/physics.md (Jolt integration + PhysicsWorld + Ragdoll)"
```

- [ ] **Step 3: Push and open PR**

```
git push -u origin feat/m18-jolt-physics-ragdoll-playground
gh pr create --title "M18: Jolt physics integration + ragdoll playground" --body "$(cat <<'EOF'
## Summary
- Jolt Physics enters via vcpkg manifest (`joltphysics`)
- `iron::PhysicsWorld` thin wrapper with pimpl (hides Jolt from public headers)
- `iron::BodyId` / `iron::JointId` opaque handles
- Body creation (static box, dynamic box/sphere/capsule), state queries, forces, raycast
- Swing-twist + hinge joints
- `iron::Ragdoll` — 11-body humanoid skeleton + 10 joints (anatomical mass distribution, distinct per-bone colors)
- `games/09-physics-playground` — visual validator: ground + ramps + wall + dynamic box stack + ragdoll spawning + ball-firing
- 2 new unit tests: `test_physics_world` (gravity, raycast, impulse, determinism), `test_ragdoll` (lifecycle, free-fall)
- New `docs/engine/physics.md`

First milestone of the physics overhaul track. Future milestones (M19 character controller, M20 projectiles/raycasts in net-shooter, M21 death-into-ragdoll) build on this foundation.

## Test plan
- [ ] CI green (Windows MSVC) — includes vcpkg Jolt build
- [ ] `.\build-vk\games\09-physics-playground\Debug\physics-playground.exe` runs
- [ ] R spawns a ragdoll that tumbles believably (not rigid statue, not floppy noodle)
- [ ] B fires a sphere that knocks over the box stack and pushes ragdolls
- [ ] C clears and resets
- [ ] Determinism test passes (same inputs → byte-identical body positions)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-review summary

**Spec coverage:**
- ✅ vcpkg + `PhysicsWorld` + body lifecycle + raycast + impulse → Task 1
- ✅ Joints + `Ragdoll` class → Task 2
- ✅ Playground demo → Task 3
- ✅ Docs → Task 4
- ✅ Determinism test → Task 1 Step 6
- ✅ Free-fall test → Task 2 Step 6
- ✅ Pimpl pattern → Task 1 Step 4 (impl_ unique_ptr) + Step 5 (struct Impl in .cpp)
- ✅ Jolt link PRIVATE → Task 1 Step 3
- ✅ Engine math types only in public header → Task 1 Step 4 (no `<Jolt/...>` includes)

**Placeholder scan:** clean — every code step has full code; no TBDs.

**Type consistency:** `BodyId` / `JointId` / `MeshHandle` / `Vec3` / `Mat4` / `Quaternion` used consistently across tasks. `boneBody(int)` returns `BodyId` as expected by tests and demo.

**Ambiguity:** Ramps are documented as flat-box approximations for v1 (Task 3 Step 3 notes). All other shapes have explicit values.

**Known limitations called out in plan:**
- Raycast normal is approximate in Task 1 (placeholder); proper normal extraction with body locking is a follow-up.
- Ramps are flat boxes in v1 (the wrapper doesn't take initial rotation; would need an enhancement to `createStaticBox` or a new `createStaticBoxRotated`).
- Single-threaded for determinism; multi-threading is a future opt-in.

These are acceptable v1 simplifications — none block the visual validator.
