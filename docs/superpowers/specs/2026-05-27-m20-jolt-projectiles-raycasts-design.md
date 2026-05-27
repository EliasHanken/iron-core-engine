# M20 — Jolt Projectiles + Raycasts in Net-Shooter (Design Spec)

**Date:** 2026-05-27
**Milestone:** M20 (third of the physics overhaul track)
**Status:** Design — awaiting implementation plan

## Goal

Replace `games/07-net-shooter`'s ad-hoc projectile + hitscan stack with Jolt-driven physics. Rockets become dynamic sphere bodies in a host-side shared world; they explode on first contact with arena geometry via a Jolt contact callback. Hitscan rifle raycasts the same world before the existing player-AABB rewind check, so bullets no longer pass through walls. The legacy `engine/game/ProjectileSim` is retired.

After M20 lands, net-shooter has unified physics for movement + projectiles + world raycasts. M21 (death-into-ragdoll) then plugs into the same physics machinery.

## Direction context

This is the third milestone in the M18-M21 physics overhaul track. M18 brought Jolt in. M19 ported character movement onto `iron::CharacterController`. M20 ports the remaining gameplay queries.

Per the M19 lineage decision, the host has per-player `PhysicsWorld` instances for character controllers (one per peer) — no player-vs-player physics interaction. M20 adds **one more** world on the host: a shared "projectile world" containing arena geometry + projectile bodies. Characters do not enter this world; their hit detection stays via the existing `LagCompensator` rewinding their game-side AABBs.

## Non-Goals

- **Direct rocket hit on a player capsule.** Rockets live in `worldShared`; player capsules live in per-peer worlds (M19). Direct hits would require unified physics or a manual sweep test — out of scope for v1. Splash radius catches anyone close enough to the wall the rocket hit.
- **Client-side rocket physics.** Clients keep the existing simple linear extrapolation for ghost rockets (`position += velocity * dt` in the render loop). Cheap, no determinism contract needed — host's `DespawnProjectileMsg` overrides on detonation.
- **Per-body gravity scale engine API.** v1 zeroes out vy each tick on the game side for rockets. A `setGravityFactor(BodyId, float)` engine helper is a tiny follow-up if it becomes painful.
- **Wire format changes.** `SpawnProjectileMsg` + `DespawnProjectileMsg` already carry everything we need. No `kGameId` bump.
- **Rocket-vs-rocket interaction semantics.** Default Jolt behavior — they collide, both explode. Niche but acceptable. Could be tuned later via layer filtering.
- **Replacing `iron::Projectile` POD.** The struct stays (it's a clean DTO used by client-side ghost rendering + the `SpawnProjectileMsg` payload). Only `tickProjectile` is gone.

## Architecture

### 1. Engine: `iron::PhysicsWorld::raycast` proper normal extraction

M18 returned `normal = {0,1,0}` as a placeholder. M20 fixes it. Inside the wrapper, after `CastRay` returns a hit, lock the body via `JPH::BodyLockRead` and call `GetWorldSpaceSurfaceNormal(subShapeID, pointOnSurface)`. Convert back to `iron::Vec3`.

No public API change. Add one sub-test to `test_physics_world.cpp` confirming the normal points up when the ray hits a horizontal box surface from above.

### 2. Engine: `iron::PhysicsWorld::onContactStarted` callback

New public method:

```cpp
struct ContactEvent {
    BodyId bodyA;
    BodyId bodyB;
    Vec3   point;     // world-space contact point
    Vec3   normal;    // world-space contact normal (pointing from A to B)
};

class PhysicsWorld {
public:
    // ... existing methods ...

    // Register a callback fired when two bodies start touching.
    // Called from inside step(); implementations should record events
    // into a queue and process them AFTER step returns. Setting a new
    // callback replaces the previous one.
    void onContactStarted(std::function<void(const ContactEvent&)>);
};
```

Implementation: a single `JPH::ContactListener` subclass registered with the `JPH::PhysicsSystem` at `init()` time. The listener's `OnContactAdded` override packages the contact into `ContactEvent` and calls the user callback (if set). Callback runs synchronously during step; the GAME is responsible for queueing rather than mutating during the call.

Test: in `test_physics_world.cpp`, register a callback, drop a dynamic box onto a static ground, step a frame, confirm the callback fired with the right body IDs.

### 3. Game-side: host's shared projectile world

Net-shooter's host gets one new `iron::PhysicsWorld worldShared` alongside the per-peer character worlds from M19. Populated at startup with the same `populateArenaCollision(worldShared, arena)` helper used in M19. **No character capsules in this world.**

```cpp
// Host-only. Created once at host startup.
iron::PhysicsWorld worldShared;
worldShared.init();
populateArenaCollision(worldShared, arena);

// Track active rockets so the host can despawn + apply splash on contact.
struct HostRocket {
    iron::BodyId  body;
    std::uint32_t projectileId;
    std::uint32_t ownerPeerId;
    double        spawnTimeSec;
};
std::unordered_map<std::uint32_t, HostRocket> hostRockets;

// Despawn queue, populated by the contact callback.
struct DespawnEvent {
    std::uint32_t projectileId;
    iron::Vec3    point;
};
std::vector<DespawnEvent> pendingDespawns;

worldShared.onContactStarted([&](const iron::ContactEvent& evt) {
    // Find which body is a rocket (could be A or B; or both if rocket-vs-rocket).
    for (auto& [id, rocket] : hostRockets) {
        if (rocket.body == evt.bodyA || rocket.body == evt.bodyB) {
            pendingDespawns.push_back({id, evt.point});
        }
    }
});
```

### 4. Host-side rocket spawn (`FireRocketMsg` handler)

```cpp
registry.registerHandler<iron::netshooter::FireRocketMsg>(
    [&](iron::ConnectionId c, const iron::netshooter::FireRocketMsg& msg) {
        if (!peers.isHost()) return;
        auto pid = peers.lookupPeerId(c);
        if (!pid) return;

        const std::uint32_t projId = nextProjectileId++;
        constexpr float kRadius = 0.10f;
        constexpr float kMass   = 0.5f;
        iron::BodyId body = worldShared.createDynamicSphere(
            iron::Vec3{msg.ox, msg.oy, msg.oz}, kRadius, kMass);
        worldShared.setVelocity(body, iron::Vec3{msg.dx, msg.dy, msg.dz});

        hostRockets[projId] = HostRocket{
            body, projId, *pid, nowSec(),
        };

        // Broadcast spawn to clients for ghost rendering.
        peers.broadcastToAll<iron::netshooter::SpawnProjectileMsg>(
            iron::netshooter::SpawnProjectileMsg{
                projId, *pid,
                msg.ox, msg.oy, msg.oz,
                msg.dx, msg.dy, msg.dz,
                nowSec(),
            });
    });
```

### 5. Host-side per-tick rocket bookkeeping

```cpp
// Inside the host's main update loop, AFTER worldShared.step(dt):

// 1. Cancel gravity per tick. Rocket velocity y is reset to its
//    initial value (modulo any contact impulses Jolt applied).
//    Simpler v1: each frame, re-set the body's velocity if we know
//    its initial. Even simpler: zero the y-component each tick.
for (const auto& [id, rocket] : hostRockets) {
    iron::Vec3 v = worldShared.velocityOf(rocket.body);  // see below
    if (v.y < 0.0f) v.y = 0.0f;  // crude — rockets don't drop
    worldShared.setVelocity(rocket.body, v);
}
// (Alternative: add `setVelocity` + a corresponding `getVelocity`
// accessor to PhysicsWorld if it doesn't exist. M18 only has
// setVelocity. v1 adds the getter as a one-line helper.)

// 2. Process despawn events from the contact listener.
for (const auto& d : pendingDespawns) {
    auto it = hostRockets.find(d.projectileId);
    if (it == hostRockets.end()) continue;
    const HostRocket& rocket = it->second;

    // Splash damage check (UNCHANGED from current code) — rewind player
    // AABBs from LagCompensator within kSplashRadius of d.point. Apply
    // damage, broadcast DamageMsg, etc.
    applySplashDamage(rocket.ownerPeerId, d.point);

    // Tell clients to render explosion + remove ghost.
    peers.broadcastToAll<iron::netshooter::DespawnProjectileMsg>(
        iron::netshooter::DespawnProjectileMsg{
            d.projectileId, d.point.x, d.point.y, d.point.z,
        });

    worldShared.destroyBody(rocket.body);
    hostRockets.erase(it);
}
pendingDespawns.clear();

// 3. Lifetime check — force-despawn rockets older than 5s.
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

> **Helper:** add `Vec3 velocityOf(BodyId) const` to `iron::PhysicsWorld` (mirror of `setVelocity`). Trivial — Jolt's `BodyInterface::GetLinearVelocity` returns the value.

### 6. Host-side hitscan world-check (`FireHitscanMsg` handler)

```cpp
registry.registerHandler<iron::netshooter::FireHitscanMsg>(
    [&](iron::ConnectionId c, const iron::netshooter::FireHitscanMsg& msg) {
        if (!peers.isHost()) return;
        auto pid = peers.lookupPeerId(c);
        if (!pid) return;

        const iron::Vec3 origin{msg.ox, msg.oy, msg.oz};
        const iron::Vec3 dir{msg.dx, msg.dy, msg.dz};
        constexpr float kMaxDist = iron::netshooter::HitscanRifle::kMaxRange;

        // Step 1 (NEW): raycast the shared world for static-world hit.
        auto worldHit = worldShared.raycast(origin, dir, kMaxDist);
        const float tWorld = worldHit.hit
            ? worldHit.t * kMaxDist          // CastRay returns 0..1 fraction; convert to distance
            : std::numeric_limits<float>::infinity();

        // Step 2 (UNCHANGED): lag-compensated player ray vs rewound AABBs.
        auto playerHit = lagComp.hitscan(*pid, origin, dir, kMaxDist,
                                          msg.viewTimeSec);
        const float tPlayer = playerHit.has_value()
            ? distance(origin, playerHit->point)
            : std::numeric_limits<float>::infinity();

        // Step 3 (NEW): pick the closer hit.
        if (tWorld <= tPlayer) {
            // Wall absorbed the shot — no damage. (Optional: muzzle FX
            // at the wall hit point — broadcast a debug-line gizmo.)
            return;
        }

        // Apply damage to the player as before.
        if (playerHit.has_value()) {
            applyHitscanDamage(*pid, playerHit->victimPeerId, playerHit->point);
        }
    });
```

### 7. Client-side: no changes for rockets

Clients still receive `SpawnProjectileMsg` (initial pos+vel) → store as `iron::Projectile` ghost → extrapolate linearly each frame. On `DespawnProjectileMsg`, render the explosion FX at the host-authoritative impact point + remove the ghost. **No physics on the client for projectiles.**

### 8. Cleanup: drop `engine/game/ProjectileSim`

After Task 2 lands and the net-shooter compiles + runs, delete:
- `engine/game/ProjectileSim.cpp`
- `tests/test_projectile_sim.cpp`

Remove their CMake entries. Keep `engine/game/ProjectileSim.h` ONLY if it defines `iron::Projectile`. If `Projectile` is only used by net-shooter game code, move the struct into `games/07-net-shooter/Projectile.h` and delete the engine header entirely.

> Pre-check during implementation: grep for `tickProjectile` and `iron::Projectile` usage outside net-shooter and tests. If neither leaks elsewhere, the engine header can go too.

## Tasks

Four subagent-friendly chunks:

1. **Engine raycast normal + contact listener** — fix `PhysicsWorld::raycast` to return the real surface normal; add `onContactStarted` callback; add `velocityOf` getter for v1 gravity-cancel. Tests in `test_physics_world.cpp`: normal-direction check + contact-callback fires.

2. **Net-shooter rocket port** — host's `worldShared` created + populated; FireRocketMsg handler spawns Jolt sphere; per-tick: step + gravity-cancel + despawn-queue processing + lifetime check; splash damage unchanged.

3. **Net-shooter hitscan world-check + ProjectileSim drop** — host raycasts `worldShared` before `LagCompensator::hitscan`; pick closer hit. Delete `engine/game/ProjectileSim.cpp` + `tests/test_projectile_sim.cpp` + their CMake lines. Move `iron::Projectile` POD into game code if no other consumers.

4. **Docs** — append M20 section to `docs/engine/physics.md`.

## Tests

`tests/test_physics_world.cpp` gains 2 sub-tests:
- **Raycast normal:** create a ground box at y=-0.5 (top at y=0). Raycast straight down from y=10. Hit normal should be ≈ `(0, 1, 0)`.
- **Contact callback fires:** register a callback that records into a captured vector. Drop a dynamic body onto a static ground. Step 60 frames at 1/60s. Expect at least one contact event between the two bodies.

Manual integration (interactive) in net-shooter:
- Fire a rocket at the floor: explodes at impact, no damage to self if outside splash radius.
- Fire a rocket at a wall: explodes, damages anyone within splash radius standing nearby.
- Fire a rocket at a corner: explodes correctly (Jolt handles edge contacts).
- Stand behind a wall, get shot at: bullet does NOT damage you (hitscan world-check works).
- Shoot a teammate standing in front of you: damage applies (no world in between).
- Fire 20 rockets rapidly: pool doesn't exhaust (`kMaxDescriptorSetsPerFrame` is at 1024 from M18 — should be fine).

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| Contact callback fires during step → callback queues into shared state | Document: callback may NOT mutate `PhysicsWorld`; it appends to a thread-safe queue (single-threaded so plain `std::vector` is fine). |
| Rocket-vs-rocket contact = both queued for despawn → erase twice | The despawn loop dedupes by checking `find()` — second iteration sees the rocket already removed and skips. |
| `Jolt::BodyLockRead` deadlock in single-threaded mode | Single-threaded; no other locker active. Safe. |
| Gravity-cancel via per-tick velocity overwrite is hacky | Acknowledged. Tiny follow-up: `setGravityFactor` engine helper. Doesn't affect gameplay. |
| Client extrapolation drifts from host's true rocket trajectory | The rocket has constant velocity (no drag, no gravity, no bouncing) — client's linear extrapolation matches the host exactly until detonation. No drift. |
| Direct-hit-on-player not detected | Acknowledged non-goal. Splash radius covers most cases. Future track. |
| `iron::Projectile` POD might be referenced by other games | Pre-check via grep. Strandbound + others don't use it (it's net-shooter-specific). Safe to relocate. |

## Verification

- **CI green** — new engine tests pass; no test regressions.
- **Solo net-shooter**: rockets explode on walls, splash damages players in range, hitscan no longer passes through walls.
- **Two-process net-shooter**: rocket spawn/despawn timing matches across host + client.
- **Solo `09-physics-playground`** (visual regression): nothing in the playground changed; ragdolls still tumble, ball-firing still works.

## Follow-ups (NOT in M20)

- **M21** — death-into-ragdoll wiring in net-shooter.
- Player-vs-projectile direct hits — requires unified physics world or game-side sweep, after M21.
- `iron::PhysicsWorld::setGravityFactor(BodyId, float)` — drop the per-tick velocity hack.
- Rocket-vs-character physics interaction (rockets push players in TF2-style rocket jumping).
- Networked rocket position updates (for high-loss links where extrapolation drifts) — not needed for constant-velocity rockets but worth knowing.
