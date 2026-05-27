# M20 Jolt Projectiles + Raycasts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `games/07-net-shooter`'s ad-hoc swept-segment projectile physics with Jolt rigid-body rockets in a host-side `worldShared`; fix hitscan to respect arena walls (currently shoots through); retire `engine/game/ProjectileSim`.

**Architecture:** Engine adds (1) a proper raycast normal extraction, (2) a `onContactStarted` callback registered against the underlying `JPH::ContactListener`, (3) a `velocityOf` getter. Net-shooter's host creates one `worldShared` (arena boxes + projectile bodies), spawns Jolt dynamic spheres in `FireRocketMsg`, despawns them via the contact listener at first wall contact, applies splash damage as before. Hitscan host-side handler raycasts `worldShared` and short-circuits if a wall is closer than the player hit. Legacy `ProjectileSim` files get deleted; the `iron::Projectile` POD relocates into game code.

**Tech Stack:** C++23, Jolt 5.5.0 (vcpkg), GameNetworkingSockets, CMake, MSVC.

---

## File Structure

### Modified
- `engine/physics/PhysicsWorld.h` — add `ContactEvent` struct + `onContactStarted` method + `velocityOf` getter
- `engine/physics/PhysicsWorld.cpp` — real raycast normal extraction; `ContactListener` subclass + member; `velocityOf` impl
- `tests/test_physics_world.cpp` — 2 new sub-tests (raycast normal direction, contact-callback fires)
- `games/07-net-shooter/main.cpp` — host's `worldShared`; rocket handler spawns Jolt body; per-tick step + contact despawn queue + lifetime cap; hitscan world-check
- `games/07-net-shooter/RocketLauncher.cpp` — `tickRocketClient` rewritten to linear advance (no `tickProjectile`)

### Deleted
- `engine/game/ProjectileSim.h`
- `engine/game/ProjectileSim.cpp`
- `tests/test_projectile_sim.cpp`

### Created
- `games/07-net-shooter/Projectile.h` — relocated `iron::Projectile` POD (now `iron::netshooter::Projectile`)

### Touched CMake
- `engine/CMakeLists.txt` — drop `game/ProjectileSim.cpp` from sources
- `tests/CMakeLists.txt` — drop `test_projectile_sim` registration

---

## Task 1: Engine — raycast normal + onContactStarted + velocityOf

**Files:**
- Modify: `engine/physics/PhysicsWorld.h`
- Modify: `engine/physics/PhysicsWorld.cpp`
- Modify: `tests/test_physics_world.cpp`

Standalone task. After this, the engine has the three new pieces and tests pass.

- [ ] **Step 1: Add `ContactEvent`, `onContactStarted`, and `velocityOf` to `PhysicsWorld.h`**

Open `engine/physics/PhysicsWorld.h`. In the `iron` namespace, after `BodyId`/`JointId` forward types but before `class PhysicsWorld`, add:

```cpp
struct ContactEvent {
    BodyId bodyA;
    BodyId bodyB;
    Vec3   point;    // world-space contact point
    Vec3   normal;   // world-space contact normal (points from bodyB into bodyA)
};
```

Inside `class PhysicsWorld`, in the `public:` section, AFTER `void destroyJoint(JointId);` and BEFORE the `engineImpl()` accessor block, add:

```cpp
    // --- Contact callback ---
    // Fires when two bodies start touching. Called DURING step(); the
    // callback MUST NOT mutate the PhysicsWorld. Record into a queue and
    // process after step() returns. Setting a new callback replaces the
    // previous one. Pass `nullptr` to disable.
    void onContactStarted(std::function<void(const ContactEvent&)> cb);
```

Also, at the top of `PhysicsWorld.h`, add `#include <functional>` next to the existing `<memory>` include.

Add `Vec3 velocityOf(BodyId) const;` to the `public:` section in the "Forces / impulses" block, immediately after `void setVelocity(BodyId, Vec3 vel);`:

```cpp
    Vec3 velocityOf(BodyId) const;
```

- [ ] **Step 2: Implement `velocityOf` + Jolt ContactListener subclass + `onContactStarted` in `PhysicsWorld.cpp`**

Open `engine/physics/PhysicsWorld.cpp`. Near the existing Jolt includes, add:

```cpp
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
```

Inside the anonymous namespace at the top of the file (where `BroadPhaseLayerInterfaceImpl` etc. live), add a `ContactListener` subclass. Place it just before the closing `}  // namespace`:

```cpp
class ContactListenerImpl final : public JPH::ContactListener {
public:
    using UserCallback = std::function<void(const ContactEvent&)>;
    void setUserCallback(UserCallback cb) { user_ = std::move(cb); }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*settings*/) override {
        if (!user_) return;
        ContactEvent evt;
        evt.bodyA = BodyId{body1.GetID().GetIndexAndSequenceNumber()};
        evt.bodyB = BodyId{body2.GetID().GetIndexAndSequenceNumber()};
        // Use the contact's "average" point — Jolt provides one or more
        // manifold points; for v1 take the first.
        if (manifold.mRelativeContactPointsOn1.size() > 0) {
            const JPH::RVec3 base = manifold.mBaseOffset;
            const JPH::Vec3 p1 = manifold.mRelativeContactPointsOn1[0];
            evt.point = Vec3{
                base.GetX() + p1.GetX(),
                base.GetY() + p1.GetY(),
                base.GetZ() + p1.GetZ(),
            };
        } else {
            evt.point = Vec3{};
        }
        evt.normal = Vec3{manifold.mWorldSpaceNormal.GetX(),
                          manifold.mWorldSpaceNormal.GetY(),
                          manifold.mWorldSpaceNormal.GetZ()};
        user_(evt);
    }
private:
    UserCallback user_;
};
```

In the `struct PhysicsWorld::Impl` definition (still in `PhysicsWorld.cpp`), add a member:

```cpp
    ContactListenerImpl contactListener;
```

In `PhysicsWorld::init()`, AFTER the `impl_->system->Init(...)` call and BEFORE the `SetGravity(...)` call, register the listener:

```cpp
    impl_->system->SetContactListener(&impl_->contactListener);
```

Add the two new public method implementations near the bottom of the file (matching the existing layout — after `raycast` and before joints, or alongside the joint section, your choice; pick the existing-style spot):

```cpp
void PhysicsWorld::onContactStarted(std::function<void(const ContactEvent&)> cb) {
    impl_->contactListener.setUserCallback(std::move(cb));
}

Vec3 PhysicsWorld::velocityOf(BodyId b) const {
    if (!b.isValid()) return {};
    const JPH::Vec3 v = impl_->system->GetBodyInterface().GetLinearVelocity(toJoltBodyId(b));
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
```

- [ ] **Step 3: Fix `raycast` normal extraction**

Find the existing `PhysicsWorld::RaycastHit PhysicsWorld::raycast(...)` implementation. The current placeholder is `out.normal = Vec3{0.0f, 1.0f, 0.0f};`. Replace the entire normal-assignment with a proper extraction via body lock:

```cpp
    // Lock the hit body to extract the real surface normal.
    {
        JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            const JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, pt);
            out.normal = Vec3{n.GetX(), n.GetY(), n.GetZ()};
        }
    }
```

(Place this block immediately after the existing `out.point = toI(pt);` line; remove the old placeholder line.)

- [ ] **Step 4: Build**

```
cmake --build build-vk --config Debug --target ironcore
```

Expected: clean. If `mRelativeContactPointsOn1`'s element type doesn't match the conversion code, check Jolt's `ContactListener.h` header in `build-vk/vcpkg_installed/x64-windows/include/Jolt/Physics/Collision/`. Jolt 5.5.0 stores them as `JPH::ContactPoints` (typedef for an inline array). The exact element access is `manifold.mRelativeContactPointsOn1[0]` returning `JPH::Vec3`.

- [ ] **Step 5: Extend `tests/test_physics_world.cpp` with two new sub-tests**

Open `tests/test_physics_world.cpp`. Just before the existing `return iron_test_result();` line at the bottom of `main()`, add two new sub-test blocks:

```cpp
    // --- Raycast normal: ray straight down onto a horizontal ground top ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});  // top at y=0
        w.step(1.0f / 60.0f);  // commit to broadphase

        auto hit = w.raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
        CHECK(hit.hit);
        // Surface normal of the top face points UP.
        CHECK(hit.normal.y > 0.9f);
    }

    // --- Contact callback fires when a dynamic box lands on a static ground ---
    {
        PhysicsWorld w; w.init();
        w.createStaticBox({0.0f, -0.5f, 0.0f}, {25.0f, 0.5f, 25.0f});
        BodyId dyn = w.createDynamicBox({0.0f, 5.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1.0f);

        int contactCount = 0;
        w.onContactStarted([&](const ContactEvent& evt) {
            // Either bodyA or bodyB should be our dynamic body (depending
            // on Jolt's ordering).
            if (evt.bodyA == dyn || evt.bodyB == dyn) {
                ++contactCount;
            }
        });

        // 3 seconds is more than enough for the box to fall and touch the
        // ground (free-fall from y=5 hits in ~1.0s).
        for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);

        CHECK(contactCount >= 1);
    }
```

- [ ] **Step 6: Build + run the test**

```
cmake --build build-vk --config Debug --target test_physics_world
ctest --test-dir build-vk -C Debug -R test_physics_world --output-on-failure
```

Expected: PASS. All previous sub-tests + the two new ones.

If "Contact callback fires" reports `contactCount == 0`: the listener might not be registered until `init()` completes. Confirm Step 2's `SetContactListener` call sits AFTER `Init`. Also, the contact callback only fires on FIRST touch; once the dynamic body settles on the ground, no more events. The loop should still see at least one event.

- [ ] **Step 7: Commit**

```
git add engine/physics/PhysicsWorld.h engine/physics/PhysicsWorld.cpp \
        tests/test_physics_world.cpp
git commit -m "M20 Task 1: PhysicsWorld raycast normal + onContactStarted + velocityOf"
```

---

## Task 2: Net-shooter host rocket port

**Files:**
- Modify: `games/07-net-shooter/main.cpp` (host-side `worldShared`, rocket spawn handler, per-tick step + despawn queue, lifetime cap)

After this task, host rockets are Jolt dynamic spheres that explode on first contact with arena geometry. Client ghost rendering is UNCHANGED (Task 3 simplifies it). Splash damage logic is preserved.

- [ ] **Step 1: Add `worldShared` + host-rocket state near the existing per-peer sim infrastructure**

Open `games/07-net-shooter/main.cpp`. Find the host-only section where `hostSims` (from M19) is declared. **Immediately after** that block, add:

```cpp
// M20 — Host-side projectile world. Contains arena geometry + active
// rocket bodies. Lives alongside the per-peer character worlds.
// Characters do NOT enter this world (preserves M19 isolation).
iron::PhysicsWorld worldShared;
worldShared.init();
populateArenaCollision(worldShared, arena);

// Host-side rocket tracking. Each entry is one in-flight rocket whose
// Jolt body lives in worldShared.
struct HostRocket {
    iron::BodyId   body;
    std::uint32_t  projectileId;
    std::uint32_t  ownerPeerId;
    double         spawnTimeSec;
};
std::unordered_map<std::uint32_t, HostRocket> hostRockets;

// Despawn queue, populated from the worldShared contact listener.
// Processed AFTER worldShared.step() each tick.
struct DespawnEvent {
    std::uint32_t projectileId;
    iron::Vec3    point;
};
std::vector<DespawnEvent> pendingDespawns;

worldShared.onContactStarted([&](const iron::ContactEvent& evt) {
    // Find the rocket body involved. If two rockets collide, both get
    // queued.
    for (auto& [id, rocket] : hostRockets) {
        if (rocket.body == evt.bodyA || rocket.body == evt.bodyB) {
            pendingDespawns.push_back({id, evt.point});
        }
    }
});
```

This block runs once at startup (it's inside `main()` along with the rest of the setup).

- [ ] **Step 2: Rewrite the `FireRocketMsg` handler (host-side)**

Find the existing handler around line 1108. It currently calls `spawnRocketHost(...)` from `RocketLauncher.cpp`, then pushes into `liveRockets`. Replace the body of the handler:

```cpp
registry.registerHandler<iron::netshooter::FireRocketMsg>(
    [&](iron::ConnectionId c, const iron::netshooter::FireRocketMsg& msg) {
        if (!peers.isHost()) return;
        auto pid = peers.lookupPeerId(c);
        if (!pid) return;
        if (!isAlive(*pid)) return;

        // Server-side cooldown gate (preserves M8.6 anti-spam).
        if (!serverRocket.cooldown.tryFire(nowSec())) return;

        const std::uint32_t projId = nextProjectileId++;
        constexpr float kRadius = 0.10f;
        constexpr float kMass   = 0.5f;

        const iron::Vec3 spawnPos{msg.ox, msg.oy, msg.oz};
        const iron::Vec3 velocity{
            msg.dx * iron::netshooter::RocketLauncher::kMuzzleSpeed,
            msg.dy * iron::netshooter::RocketLauncher::kMuzzleSpeed,
            msg.dz * iron::netshooter::RocketLauncher::kMuzzleSpeed,
        };

        iron::BodyId body = worldShared.createDynamicSphere(spawnPos, kRadius, kMass);
        worldShared.setVelocity(body, velocity);

        hostRockets[projId] = HostRocket{body, projId, *pid, nowSec()};

        peers.broadcastToAll<iron::netshooter::SpawnProjectileMsg>(
            iron::netshooter::SpawnProjectileMsg{
                projId, *pid,
                spawnPos.x, spawnPos.y, spawnPos.z,
                velocity.x, velocity.y, velocity.z,
                nowSec(),
            });
    });
```

> **Note on `isAlive(*pid)` and `serverRocket`:** verify both already exist in main.cpp (`isAlive` is the alive-check helper; `serverRocket` is the host's `RocketLauncher` member for cooldown). If they don't, look at how the existing handler did the cooldown check and copy the pattern. Don't invent new infrastructure.

Find the existing `liveRockets` declaration (around line 777) and **delete it** — `hostRockets` replaces it.

- [ ] **Step 3: Host per-tick rocket bookkeeping**

Find the host's main update loop (where it ticks the predictor, runs `LagCompensator::push(...)`, etc.) The existing code has a `for (auto& live : liveRockets)` block calling `tickRocketHost`. **Replace that entire block** with:

```cpp
// M20 — Step worldShared once per host tick. The ContactListener
// callback populates `pendingDespawns`. We then process it inline.
worldShared.step(static_cast<float>(deltaSec));

// Cancel gravity by zeroing the y-component if it has fallen below
// zero. Rockets are kinematic-feeling (constant velocity, no gravity).
for (const auto& [id, rocket] : hostRockets) {
    iron::Vec3 v = worldShared.velocityOf(rocket.body);
    if (v.y < 0.0f) {
        v.y = 0.0f;
        worldShared.setVelocity(rocket.body, v);
    }
}

// Process contact-driven despawns.
for (const auto& d : pendingDespawns) {
    auto it = hostRockets.find(d.projectileId);
    if (it == hostRockets.end()) continue;  // already removed
    const HostRocket& rocket = it->second;

    // Splash damage at the contact point (preserves M8.6 logic).
    for (const auto& [vpid, _] : authStates) {
        if (!isAlive(vpid)) continue;
        const auto aabb = lagComp.aabbAt(vpid, nowSec(),
                                          iron::netshooter::kPlayerHalfExtents);
        if (!aabb) continue;
        if (!iron::sphereOverlapAabb(d.point,
                                       iron::netshooter::RocketLauncher::kSplashRadius,
                                       *aabb)) continue;
        // Compute damage (linear falloff over splash radius).
        const float cx = std::clamp(d.point.x, aabb->min.x, aabb->max.x);
        const float cy = std::clamp(d.point.y, aabb->min.y, aabb->max.y);
        const float cz = std::clamp(d.point.z, aabb->min.z, aabb->max.z);
        const float dx = d.point.x - cx;
        const float dy = d.point.y - cy;
        const float dz = d.point.z - cz;
        const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        const float t = std::clamp(dist / iron::netshooter::RocketLauncher::kSplashRadius,
                                    0.0f, 1.0f);
        const int dmg = static_cast<int>(
            std::round(iron::netshooter::RocketLauncher::kCenterDamage * (1.0f - t)));
        if (dmg <= 0) continue;

        applyDamageHost(rocket.ownerPeerId, vpid,
                        static_cast<std::uint16_t>(dmg));
    }

    // Broadcast despawn for clients (renders explosion + removes ghost).
    peers.broadcastToAll<iron::netshooter::DespawnProjectileMsg>(
        iron::netshooter::DespawnProjectileMsg{
            d.projectileId, d.point.x, d.point.y, d.point.z,
        });

    worldShared.destroyBody(rocket.body);
    hostRockets.erase(it);
}
pendingDespawns.clear();

// Lifetime cap: force-despawn rockets older than 5s.
for (auto it = hostRockets.begin(); it != hostRockets.end(); ) {
    if (nowSec() - it->second.spawnTimeSec > 5.0) {
        const auto pos = worldShared.bodyPosition(it->second.body);
        peers.broadcastToAll<iron::netshooter::DespawnProjectileMsg>(
            iron::netshooter::DespawnProjectileMsg{
                it->first, pos.x, pos.y, pos.z,
            });
        worldShared.destroyBody(it->second.body);
        it = hostRockets.erase(it);
    } else {
        ++it;
    }
}
```

> **Replace `applyDamageHost(...)` with whatever function the existing code uses** to apply damage and broadcast `DamageMsg`. Search for `DamageMsg` constructions in the current handler around line 1108+ and follow that pattern. Don't invent a new function — use the one already there.

- [ ] **Step 4: Update the per-peer-leaving cleanup**

The existing code around line 908 (`onPeerLeft`) calls `lagComp.forgetPeer(...)` and erases from `authStates` / `hostSims`. **Also iterate `hostRockets`** and destroy any rocket owned by the leaving peer — they'd otherwise hang in `worldShared` forever:

```cpp
// After the existing lagComp.forgetPeer / authStates.erase / hostSims.erase:
for (auto it = hostRockets.begin(); it != hostRockets.end(); ) {
    if (it->second.ownerPeerId == pid) {
        worldShared.destroyBody(it->second.body);
        it = hostRockets.erase(it);
    } else {
        ++it;
    }
}
```

- [ ] **Step 5: Build**

```
cmake --build build-vk --config Debug --target net-shooter
```

Expected: clean compile. If `populateArenaCollision` is undefined: it was added in M19 — should still be in main.cpp's anonymous namespace from the M19 port.

If `serverRocket` / `applyDamageHost` / `isAlive` don't compile: those names may differ. Grep for the existing rocket handler around line 1108 and copy the call sites verbatim.

- [ ] **Step 6: Solo smoke test**

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Expected:
- Fire a rocket (`2` to select rocket, LMB to fire): rocket flies forward at constant velocity (no gravity), explodes on first wall.
- Stand near a wall, shoot at it: rocket explodes, you take splash damage if within `kSplashRadius`.
- Fire rockets rapidly: cooldown gates them; no crash.

If rockets DROP (gravity not cancelled): confirm the per-tick `v.y < 0.0f` fix is inside the loop iterating `hostRockets`. If rockets pass through walls: confirm the contact listener is fired (add a temporary `iron::Log::info` inside the callback to verify; remove before commit).

- [ ] **Step 7: Commit**

```
git add games/07-net-shooter/main.cpp
git commit -m "M20 Task 2: net-shooter host rockets onto Jolt (worldShared + contact despawn)"
```

---

## Task 3: Net-shooter hitscan world-check + retire ProjectileSim

**Files:**
- Modify: `games/07-net-shooter/main.cpp` (hitscan handler world-check)
- Modify: `games/07-net-shooter/RocketLauncher.cpp` (`tickRocketClient` no longer calls `tickProjectile`)
- Create: `games/07-net-shooter/Projectile.h` (relocated POD)
- Delete: `engine/game/ProjectileSim.h`, `engine/game/ProjectileSim.cpp`, `tests/test_projectile_sim.cpp`
- Modify: `engine/CMakeLists.txt`, `tests/CMakeLists.txt`
- Modify: any file that included `#include "game/ProjectileSim.h"` → swap to `games/07-net-shooter/Projectile.h`

After this task, hitscan no longer passes through walls, and the legacy `ProjectileSim` engine module is gone.

- [ ] **Step 1: Hitscan world-check in `FireHitscanMsg` handler**

In `games/07-net-shooter/main.cpp`, find the `FireHitscanMsg` handler around line 1055. The current code calls `lagComp.hitscan(...)` and applies damage on hit. Wrap this with a world-raycast pre-check:

```cpp
registry.registerHandler<iron::netshooter::FireHitscanMsg>(
    [&](iron::ConnectionId c, const iron::netshooter::FireHitscanMsg& msg) {
        if (!peers.isHost()) return;
        auto pid = peers.lookupPeerId(c);
        if (!pid) return;
        if (!isAlive(*pid)) return;
        if (!serverRifle.cooldown.tryFire(nowSec())) return;

        const iron::Vec3 origin{msg.ox, msg.oy, msg.oz};
        const iron::Vec3 dir{msg.dx, msg.dy, msg.dz};
        constexpr float kMaxDist = iron::netshooter::HitscanRifle::kMaxRange;

        // M20 — Raycast worldShared first. If a wall is closer than any
        // player hit, the bullet stops at the wall (no damage).
        const auto worldHit = worldShared.raycast(origin, dir, kMaxDist);
        const float tWorld = worldHit.hit
            ? worldHit.t * kMaxDist
            : std::numeric_limits<float>::infinity();

        // Existing lag-compensated player ray.
        auto playerHit = lagComp.hitscan(*pid, origin, dir, kMaxDist,
                                          msg.viewTimeSec,
                                          iron::netshooter::kPlayerHalfExtents);
        const float tPlayer = playerHit.has_value()
            ? iron::length(iron::Vec3{playerHit->point.x - origin.x,
                                       playerHit->point.y - origin.y,
                                       playerHit->point.z - origin.z})
            : std::numeric_limits<float>::infinity();

        if (tWorld <= tPlayer) {
            // Wall absorbed the shot — no damage.
            return;
        }

        if (!playerHit.has_value()) return;

        // Apply damage to the player (unchanged from existing handler).
        applyDamageHost(*pid, playerHit->victimPeerId,
                        iron::netshooter::HitscanRifle::kDamage);
    });
```

> **Replace `serverRifle.cooldown.tryFire(...)`, `isAlive(...)`, `applyDamageHost(...)`, and the exact `HitscanRifle::kDamage` constant with whatever the existing handler uses.** Don't invent. Search the original handler for the exact calls.

`iron::length(Vec3)` — verify it exists in `engine/math/Vec.h`. If not, inline the `std::sqrt(dx*dx + dy*dy + dz*dz)` computation:

```cpp
const float dx = playerHit->point.x - origin.x;
const float dy = playerHit->point.y - origin.y;
const float dz = playerHit->point.z - origin.z;
const float tPlayer = playerHit.has_value() ? std::sqrt(dx*dx + dy*dy + dz*dz)
                                             : std::numeric_limits<float>::infinity();
```

- [ ] **Step 2: Relocate `Projectile` POD into the game**

Create `games/07-net-shooter/Projectile.h`:

```cpp
#pragma once

#include "math/Vec.h"
#include <cstdint>

namespace iron::netshooter {

// Minimal POD for tracking a rocket's state. Used by:
//  - client-side `ghostRockets` (rendered as a sphere flying through
//    the arena based on last-known position + velocity; advanced via
//    linear extrapolation each frame)
//  - the `SpawnProjectileMsg` payload (initial pos+vel from host).
// Host no longer uses this struct — its rockets live in `worldShared`
// as Jolt dynamic spheres (see `HostRocket` in main.cpp).
struct Projectile {
    std::uint32_t id = 0;
    std::uint32_t ownerPeerId = 0;
    Vec3 position{};
    Vec3 velocity{};
    double spawnTimeSec = 0.0;
    bool alive = true;
};

}  // namespace iron::netshooter
```

- [ ] **Step 3: Rewrite `tickRocketClient` to linear advance**

Open `games/07-net-shooter/RocketLauncher.cpp`. The existing `tickRocketClient` calls `tickProjectile` for world-collision. Replace it with linear advance only:

```cpp
void tickRocketClient(Projectile& ghost, float dt,
                      std::span<const Aabb> /*worldBoxes*/) {
    if (!ghost.alive) return;
    // M20 — clients no longer simulate collision. Host owns the
    // authoritative trajectory; clients linearly extrapolate the rocket
    // until DespawnProjectileMsg arrives.
    ghost.position.x += ghost.velocity.x * dt;
    ghost.position.y += ghost.velocity.y * dt;
    ghost.position.z += ghost.velocity.z * dt;
}
```

(Keep the `std::span<const Aabb>` parameter to avoid touching every call site in main.cpp — the parameter is unused but the signature is preserved. Optional cleanup: remove the parameter and drop the now-unused argument at the one call site. Your call; both are fine.)

Drop the include of `engine/game/ProjectileSim.h` from `RocketLauncher.cpp` and `RocketLauncher.h`. Replace with `#include "Projectile.h"`.

Also drop `tickRocketHost` from `RocketLauncher.cpp` — the host's rocket logic moved into `main.cpp` in Task 2. If `RocketLauncher.h` still declares `tickRocketHost`, remove the declaration there too.

- [ ] **Step 4: Swap includes in `main.cpp`**

In `games/07-net-shooter/main.cpp`, find `#include "game/ProjectileSim.h"` and replace with:

```cpp
#include "Projectile.h"
```

References to `iron::Projectile` become `iron::netshooter::Projectile`. Update all such references in main.cpp (search for `iron::Projectile` and update). For RocketLauncher.cpp/.h, same swap — `Projectile` is now in the `iron::netshooter` namespace (already where the rest of net-shooter's types live).

- [ ] **Step 5: Delete the obsolete engine files**

```
git rm engine/game/ProjectileSim.h engine/game/ProjectileSim.cpp tests/test_projectile_sim.cpp
```

- [ ] **Step 6: Drop CMake entries**

Open `engine/CMakeLists.txt`. Find the line `game/ProjectileSim.cpp` in the `ironcore` STATIC sources block. Delete it.

Open `tests/CMakeLists.txt`. Find the line `iron_add_test(test_projectile_sim test_projectile_sim.cpp)`. Delete it.

- [ ] **Step 7: Build**

```
cmake --build build-vk --config Debug --target net-shooter
```

Expected: clean compile. If you missed an `iron::Projectile` → `iron::netshooter::Projectile` rename, you'll get a "no member named Projectile in iron" error pointing to the spot.

- [ ] **Step 8: Solo + networked smoke test**

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Expected:
- **Hitscan no longer passes through walls.** Stand behind a wall: a teammate cannot kill you with the rifle.
- **Rockets still work** (Task 2 unchanged): fly, explode on walls, splash damages players nearby.
- No regressions in respawn, scoring, kill feed, ammo, etc.

In two PowerShell windows (host + client):
- Both players can shoot each other in open arena.
- A player behind a wall is safe from hitscan.
- Rocket splashes through walls if the rocket explodes on the other side of the wall (current splash logic is geometry-blind by design — preserved for v1).

- [ ] **Step 9: Commit**

```
git add games/07-net-shooter/main.cpp games/07-net-shooter/RocketLauncher.h \
        games/07-net-shooter/RocketLauncher.cpp games/07-net-shooter/Projectile.h \
        engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M20 Task 3: hitscan world-check + retire engine/game/ProjectileSim"
```

(`git rm` from Step 5 stages the deletions automatically — they get included in this commit.)

---

## Task 4: Docs

**Files:**
- Modify: `docs/engine/physics.md`

- [ ] **Step 1: Append M20 section to `docs/engine/physics.md`**

After the existing M19 section, append:

```markdown
## M20 — Projectiles + raycasts on Jolt

The engine's `iron::PhysicsWorld` gained three pieces:

- `RaycastHit::normal` now contains the real surface normal (M18 was a
  placeholder). Extracted via `BodyLockRead` + `GetWorldSpaceSurfaceNormal`.
- `Vec3 velocityOf(BodyId) const` accessor — mirror of `setVelocity`.
- `void onContactStarted(std::function<void(const ContactEvent&)>)`
  registers a callback fired during `step()` when two bodies start
  touching. The callback MUST NOT mutate the `PhysicsWorld` — record into
  a queue and process events after step returns. Implementation: a
  single `JPH::ContactListener` subclass owned by `PhysicsWorld::Impl`.

```cpp
iron::PhysicsWorld world; world.init();
world.onContactStarted([&](const iron::ContactEvent& evt) {
    // evt.bodyA, evt.bodyB, evt.point, evt.normal
});
```

### Net-shooter's projectile architecture

Net-shooter's host gained one shared `worldShared` (alongside the
per-peer character worlds from M19). It contains:
- The static arena geometry (same `populateArenaCollision` helper).
- Active rocket bodies — Jolt dynamic spheres (radius 0.10m, mass 0.5kg).

**Gravity is cancelled per tick** by clamping each rocket's vy to ≥ 0.
Rockets fly in straight lines at constant velocity and explode on first
arena contact. A `setGravityFactor(BodyId, float)` engine helper would
be cleaner — flagged as a small follow-up.

Per host tick:
1. `worldShared.step(dt)` advances all rocket bodies.
2. `ContactListener` populates `pendingDespawns` for any rocket that
   touched arena geometry (or another rocket).
3. Game loop drains `pendingDespawns`: applies splash damage against
   rewound player AABBs (unchanged from M8.6 splash logic), broadcasts
   `DespawnProjectileMsg`, destroys the rocket body.
4. Lifetime cap force-despawns rockets older than 5s.

Clients **do not run physics for rockets**. They store
`iron::netshooter::Projectile` ghosts updated by `SpawnProjectileMsg`,
extrapolate linearly each frame, and replace with explosion FX on
`DespawnProjectileMsg`. The rocket has constant velocity → no drift.

### Hitscan world-check

The host's `FireHitscanMsg` handler now raycasts `worldShared` BEFORE
calling `LagCompensator::hitscan`. If a wall is closer than any
lag-compensated player AABB, the bullet stops at the wall (no damage).
This fixes the long-standing "bullets pass through walls" bug.

### Retirement

`engine/game/ProjectileSim.{h,cpp}` and `tests/test_projectile_sim.cpp`
are gone. The `iron::Projectile` POD relocated to
`games/07-net-shooter/Projectile.h` as `iron::netshooter::Projectile`
— still used by client-side ghost rendering and the
`SpawnProjectileMsg` payload.

### What's next

- **M21** — Death-into-ragdoll wiring in net-shooter. When a player's
  HP drops to 0, swap their character for a spawned `iron::Ragdoll` at
  the death position. Mostly game-side; the ragdoll machinery already
  exists from M18.
- Future: player-vs-projectile direct hits (rocket capsule overlap →
  immediate detonation at full damage). Out of v1 because capsules and
  rocket bodies live in different worlds.
- Future: `iron::PhysicsWorld::setGravityFactor(BodyId, float)` —
  replace the per-tick velocity-clamp hack.
```

- [ ] **Step 2: Commit**

```
git add docs/engine/physics.md
git commit -m "M20 Task 4: docs/engine/physics.md M20 section"
```

- [ ] **Step 3: Push + open PR**

```
git push -u origin feat/m20-jolt-projectiles-raycasts
gh pr create --title "M20: Jolt projectiles + raycasts in net-shooter" --body "$(cat <<'EOF'
## Summary
- `iron::PhysicsWorld::raycast` now returns the real surface normal (M18 had placeholder)
- `iron::PhysicsWorld::onContactStarted` callback (JPH::ContactListener)
- `iron::PhysicsWorld::velocityOf` getter
- Net-shooter host gained `worldShared` (arena + projectile bodies). Rockets are Jolt dynamic spheres (gravity cancelled per tick) that explode on first contact via the listener callback.
- Hitscan rifle now raycasts `worldShared` before `LagCompensator::hitscan` — **bullets no longer pass through walls** (fixes long-standing gameplay bug).
- `engine/game/ProjectileSim` retired; `iron::Projectile` POD relocated to `games/07-net-shooter/Projectile.h`.
- 2 new engine sub-tests (raycast normal direction, contact callback fires).

Third milestone of the physics overhaul. M21 (death-into-ragdoll) is next.

## Test plan
- [ ] CI green (Windows MSVC) — including the 2 new sub-tests
- [ ] Solo net-shooter: rockets explode on walls + splash damage; rifle no longer shoots through walls
- [ ] Two-process net-shooter: rocket spawn/despawn timing matches across host + client
- [ ] No regression in 09-physics-playground (M18 still works)

## Known v1 limitations
- Per-tick velocity clamp instead of a proper `setGravityFactor` engine helper
- No direct-hit-on-player detection (rockets are in `worldShared`, capsules in per-peer worlds) — splash still catches players

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- Engine raycast normal fix → Task 1 Step 3
- `onContactStarted` callback → Task 1 Step 2
- `velocityOf` getter → Task 1 Steps 1–2
- Host `worldShared` setup → Task 2 Step 1
- Rocket spawn as Jolt sphere → Task 2 Step 2
- Per-tick step + gravity-cancel + despawn-queue processing + lifetime cap → Task 2 Step 3
- Cleanup on peer-leave → Task 2 Step 4
- Hitscan world-check → Task 3 Step 1
- `ProjectileSim` retirement → Task 3 Steps 5–6
- `Projectile` POD relocation → Task 3 Step 2
- Client `tickRocketClient` linear-advance rewrite → Task 3 Step 3
- 2 new engine sub-tests → Task 1 Step 5
- Docs → Task 4

**Placeholder scan:** clean — every code step has actual code. Names like `applyDamageHost`/`isAlive`/`serverRifle`/`serverRocket` are flagged with "verify by reading existing handler" notes rather than invented signatures.

**Type consistency:**
- `BodyId`, `JointId` (M18), `ContactEvent` (new): consistent.
- `HostRocket` struct used in Tasks 2 and (implicitly) referenced in Task 3 — same shape.
- `worldShared` (host-only state) and `localWorld`/`hostSims` (M19 names) coexist without conflict.
- `iron::netshooter::Projectile` (Task 3) replaces `iron::Projectile` everywhere it was referenced in the game.

**Known risks documented:** gravity-cancel hack, no direct-hit detection, splash through walls.
