# Engine physics

The engine integrates [Jolt Physics](https://github.com/jrouwe/JoltPhysics) as
its rigid-body simulation backend, wrapped behind a thin engine interface
(`iron::PhysicsWorld`) that hides Jolt types from public headers. Higher-level
constructs — ragdolls, character controllers, projectiles — live on top.

## Why Jolt

The selection came down to Jolt vs PhysX, with Jolt winning on every axis that
mattered for this engine:

- **Deterministic by default.** Jolt's simulation is byte-deterministic across
  runs and machines (same architecture) when configured single-threaded. This
  is a hard requirement for the server-authoritative networking model with
  client-side prediction — see [[networking]]. PhysX is *not* deterministic
  across versions or hardware without significant care.
- **Modern C++17.** Compiles cleanly under MSVC, integrates via vcpkg
  (`joltphysics` port) with no SDK installer. PhysX needs a separate SDK pull
  and a heavier build.
- **Best-in-class character controller.** Jolt's
  `Character`/`CharacterVirtual` is what M19 will use. It handles slopes,
  stairs, moving platforms, and crouch-state changes cleanly out of the box.
- **MIT license.** No commercial-use carve-outs, no per-platform fees.
- **AAA-proven.** Horizon Forbidden West and Horizon Call of the Mountain
  ship on it. Confidence-builder for the long-haul roadmap.

PhysX is the obvious runner-up, but its standout features — GPU acceleration,
cloth, the PhysX Visual Debugger — don't apply to this engine's scope. Pure
CPU rigid-body + character + raycast is exactly Jolt's sweet spot.

## API surface — `iron::PhysicsWorld`

`engine/physics/PhysicsWorld.h` is the single public header. Game code never
includes a `<Jolt/...>` header — the wrapper uses pimpl
(`PhysicsWorld::Impl`) so all Jolt types stay inside the .cpp.

```cpp
#include "physics/PhysicsWorld.h"

iron::PhysicsWorld world;
world.init();

const auto ground = world.createStaticBox({0, -0.5f, 0}, {50, 0.5f, 50});
const auto box    = world.createDynamicBox({0,  5,    0}, {0.5f, 0.5f, 0.5f},
                                            /*mass*/ 10.0f);

while (running) {
    world.step(1.0f / 60.0f);
    const Mat4 t = world.bodyTransform(box);
    renderer->submit({mesh, shader, tex, t, mat});
}

world.shutdown();
```

Bodies are referenced by opaque `iron::BodyId` (and joints by `iron::JointId`)
handles — small POD structs holding an internal `uint32_t`. `kInvalidBody`
and `kInvalidJoint` are sentinel constants; both handles have an `isValid()`
method. Game code stores these but never inspects them.

The full surface:

- **Bodies** — `createStaticBox`, `createDynamicBox`, `createDynamicSphere`,
  `createDynamicCapsule`, `destroyBody`. Each returns a `BodyId`.
- **State queries** — `bodyPosition`, `bodyRotation`, `bodyTransform`
  (combined `Mat4`), `isBodyAlive`.
- **Forces / impulses** — `applyImpulse`, `applyForceAtPoint`, `setVelocity`.
- **Raycast** — `raycast(origin, direction, maxDistance)` returns a
  `RaycastHit{hit, body, point, normal, t}`. Used for projectiles and
  line-of-sight queries. Note this is the *physics* raycast — the existing
  `iron::math::Ray` and its non-physics intersection helpers remain for
  geometric queries that don't involve registered bodies.
- **Joints** — `createSwingTwistJoint` (for spine/shoulder/hip-style limits:
  a cone of allowed twist-axis directions + a bounded twist angle around
  it) and `createHingeJoint` (elbow/knee — one axis, signed min/max angle).
  `destroyJoint` reverses either.
- **Stepping** — `step(dt)` advances the simulation. Pick a fixed `dt`
  (typically `1.0f / 60.0f`); deterministic results require fixed steps.

Threading: `PhysicsWorld` is single-threaded externally — `step` must be
called from one thread. Jolt's internal job system runs on its own.

## API surface — `iron::Ragdoll`

`engine/physics/Ragdoll.h` builds an 11-bone humanoid skeleton on top of
`PhysicsWorld`. Bones are dynamic boxes; joints are 4 swing-twist (neck,
spine, two shoulders, two hips — actually 5 in current layout) and 4 hinges
(two elbows + two knees), totalling 10 joints. Mass is distributed
anatomically: the torso carries ~43% of the total, the head ~7%, the rest
split across arms and legs.

```cpp
iron::Ragdoll rd;
iron::RagdollSpec spec{ .totalHeight = 1.8f, .mass = 75.0f };
rd.spawn(world, spec, /*position*/ {0, 3, 0});

// Per frame, after world.step(dt):
for (int i = 0; i < rd.boneCount(); ++i) {
    const Mat4   t  = rd.boneTransform(i);
    const Vec3   h  = rd.boneHalfExtents(i);
    const Vec3   c  = rd.boneColor(i);  // distinct per-bone for readability
    renderer->submit({boxMesh, shader, whiteTex,
                      t * Mat4::scale(h * 2), Material{.albedo = c}});
}

rd.despawn(world);  // destroys joints + bodies in correct order
```

Bones are addressable by index constants for code that wants to apply
targeted forces or attach things: `kHead`, `kTorso`, `kHips`, `kUpperArmL`,
`kForearmL`, `kUpperArmR`, `kForearmR`, `kUpperLegL`, `kLowerLegL`,
`kUpperLegR`, `kLowerLegR`. The `boneBody(idx)` accessor returns the
underlying `BodyId` so callers can apply impulses (e.g. shotgun blast,
explosion knockback) directly.

`isSpawned()` is a cheap inspector — useful for "respawn if missing" logic.

## Determinism contract

`PhysicsWorld::init` configures Jolt for byte-deterministic simulation:
single-threaded job system, default `cMaxPhysicsJobs` and
`cMaxPhysicsBarriers`, and Jolt's built-in deterministic flags. The
guarantee is:

> Two `PhysicsWorld` instances on the same architecture, created with the
> same body sequence and stepped with the same `dt` sequence, produce
> identical body states bit-for-bit.

This is what makes the planned shooter rewrite (M20+) viable. The host runs
the authoritative simulation; clients run a local copy and predict ahead with
their own inputs. When the host's authoritative snapshot arrives, the client
compares its predicted state at that input id against the authority — if
they match (the common case), nothing visible happens. If they diverge, the
client snaps to authority and replays subsequent inputs. None of that works
if the simulation can drift on its own. See [[networking]] for the
prediction pattern this layers on top of (`iron::PredictionEngine`).

The unit test `tests/test_physics_world.cpp` exercises the contract: same
seed, same impulses, two worlds — bodies end up in identical positions to
the last bit.

## Visual validator: `games/09-physics-playground`

Vulkan-only demo. Builds under `IRON_RENDER_BACKEND=vulkan`; gated off
under OpenGL via the game's `CMakeLists.txt`. It is not a game — it's a
sandbox the developer (or a code reviewer) eyeballs to confirm that the
physics integration looks alive.

```
cmake --build build-vk --target physics-playground --config Debug
.\build-vk\games\09-physics-playground\Debug\physics-playground.exe
```

The scene: ground plane, a wall, four flat-box "ramps" laid out at the
cardinal directions (see note below), and a small stack of dynamic boxes.
Controls:

- `R` — spawn a ragdoll a few meters above the stack. Should tumble
  believably: limbs flop, joints respect their swing/twist limits, the
  thing settles into a pile instead of standing upright or going noodly.
- `B` — fire a sphere from the camera in the look direction. Knocks over
  the stack, scatters ragdolls, drains kinetic energy through joint
  constraints.
- `C` — clear all spawned bodies + ragdolls, reset to initial scene.
- `WASD + mouse`, `Space/Ctrl` — free-fly camera. `ESC` quits.

The "ramps" are flat axis-aligned boxes in v1 — true tilted ramps require
`PhysicsWorld` body creation to accept an initial rotation, which is a
small future enhancement. The current shape is sufficient for the visual
"does gravity feel right" check the playground exists for.

## What's next

This is the first milestone of the physics overhaul track. Planned follow-ups:

- **M19** — `iron::CharacterController` built on Jolt's `CharacterVirtual`.
  Replaces the bespoke movement code in net-shooter with something that
  handles slopes, stairs, and crouching properly.
- **M20** — projectiles + raycasts in net-shooter, ported to physics.
  Rockets become dynamic spheres; hitscan uses `PhysicsWorld::raycast`
  on the authoritative server with Jolt's collision shapes instead of the
  current hand-rolled AABB sweep.
- **M21** — death-into-ragdoll. When a player dies in net-shooter, swap
  the character capsule for a fresh `iron::Ragdoll` and apply the killing
  impulse so the body reacts to how it was killed.

The existing `iron::math::Ray` and its analytic intersection helpers stay
put for non-physics queries (frustum culling debug, gizmo picking,
geometric utilities). `PhysicsWorld::raycast` is for queries against
registered bodies only.

## M19 — Capsule character controller

`iron::CharacterController` (`engine/physics/CharacterController.{h,cpp}`)
wraps `JPH::CharacterVirtual` — Jolt's purpose-built kinematic character
controller. One instance owns one capsule registered with a
`PhysicsWorld`.

Usage:

```cpp
iron::PhysicsWorld world; world.init();
// ... populate world with static collision ...

iron::CharacterController player;
iron::CharacterControllerConfig cfg;   // 0.3m radius, 0.9m halfHeight, jump 5.5 m/s
player.create(world, cfg, /*footPosition=*/ {0, 0, 0});

// Per frame:
player.update(dt, iron::Vec3{vx, 0, vz}, jumpPressedThisFrame);
world.step(dt);

iron::Vec3 footPos = player.footPosition();
iron::Vec3 vel     = player.velocity();
bool grounded      = player.isGrounded();
```

The wrapper owns a `JPH::Ref<CharacterVirtual>`; lifetime ends with
`destroy(world)` or the wrapper's destructor. Direct mutators
(`setFootPosition`, `setVelocity`) exist for reconciliation replay — the
`simulate` lambda in net-shooter restores controller state at the start
of every call so `PredictionEngine` replays work correctly.

**`PhysicsWorld` lifetime contract:** any `CharacterController` created
with a `PhysicsWorld&` must be destroyed (or its owning world destroyed)
before the world itself goes out of scope. The controller borrows a
`PhysicsWorld*` internally for temp-allocator access during `update()`.

### Bridge: how the controller reaches into `PhysicsWorld`

`PhysicsWorld.h` exposes a public `Impl* engineImpl()` accessor — its
opaque forward-declared `struct Impl;` lets other engine TUs reach the
underlying `JPH::PhysicsSystem` without leaking Jolt headers to game
code. `PhysicsWorld.cpp` defines two bridge functions in
`iron::internal`: `getPhysicsSystem(PhysicsWorld&)` and
`getTempAllocator(PhysicsWorld&)`. `CharacterController.cpp` declares
both as extern and calls them. This pattern generalizes to future
engine subsystems that need raw access (joints, constraints, custom
queries).

### Per-player physics worlds in net-shooter (v1)

Net-shooter creates one `PhysicsWorld` + one `CharacterController` per
simulated player. The client owns one (its local predicted player); the
host owns one per peer (in a `HostPlayerSim` struct: world + controller
+ a captured `simulateFn`). Each world contains the arena's static AABBs
+ exactly one character. **Players do not collide with each other in
v1** — that's a future track. Hitscan + rocket splash still resolve
hits via the existing `LagCompensator` against the same arena AABBs and
player AABBs from the game-side history.

The shape of the static arena collision is a literal port of the
existing `iron::netshooter::Arena::boxes` vector — each `Aabb` becomes a
`world.createStaticBox(center, halfExtents)` call at startup via the
game-side `populateArenaCollision` helper.

### Wire-format change (M19)

`PlayerInputMsg` was renamed `dx/dy/dz` → `vx/vy/vz` (semantic flip:
world-space velocity in m/s, not per-tick deltas) and gained a `jump`
byte. `vy` is reserved (unused, always 0 on the wire).

`AuthorityPositionMsg` gained `vx/vy/vz` (velocity) and `grounded`
(uint8). Required so client reconciliation can restore full character
state, not just position — otherwise the controller's velocity drifts
between predicted and authoritative.

`kGameId` bumped from `0x4E535452` ("NSTR") to `0x4E535453` ("NSTS").
Old clients cannot connect to new hosts; the PeerManager Hello
handshake (M8.4) validates exact equality.

### Determinism

Per-player worlds means each `simulate(state, input, dt)` call is
self-contained — the only "world" the call advances is the local
controller's. `PredictionEngine` reconciliation replay calls
`simulate` N times in a row; each call restores controller state at
entry. As long as the static geometry (arena) is identical across
client and host (it is — same procedural seed), simulate is
deterministic.

### What's next

- M20 — projectile rigid bodies + raycasts in net-shooter. Net-shooter
  rockets → Jolt rigid bodies; hitscan → Jolt raycasts.
- M21 — death-into-ragdoll wiring (uses M18's `iron::Ragdoll`).
- Future: player-vs-player physics collision (single shared world,
  player-layer filtering), capsule visual rendering, movement polish
  (crouch, sprint, slide).

## M20 — Projectiles on Jolt

`iron::PhysicsWorld` gained three pieces:

- `RaycastHit::normal` now contains the real surface normal — M18 was a
  placeholder. Implementation: `JPH::BodyLockRead` + `GetWorldSpaceSurfaceNormal`.
- `Vec3 velocityOf(BodyId) const` accessor — mirror of `setVelocity`.
- `void onContactStarted(std::function<void(const ContactEvent&)>)`
  registers a callback fired during `step()` when two bodies start
  touching. The callback MUST NOT mutate the `PhysicsWorld` — record
  into a queue and process events after step returns. Implementation:
  one `JPH::ContactListener` subclass owned by `PhysicsWorld::Impl`.

```cpp
iron::PhysicsWorld world; world.init();
world.onContactStarted([&](const iron::ContactEvent& evt) {
    // evt.bodyA, evt.bodyB, evt.point, evt.normal
});
```

### Net-shooter's projectile architecture (M20)

Net-shooter's host gained one shared `worldShared` (alongside the
per-peer character worlds from M19). It contains:
- The static arena geometry (same `populateArenaCollision` helper).
- Active rocket bodies — Jolt dynamic spheres (radius 0.10m, mass 0.5kg).

**Gravity is cancelled per tick** by clamping each rocket's vy ≥ 0.
Rockets fly in straight lines at constant velocity and explode on first
arena contact. A `setGravityFactor(BodyId, float)` engine helper would
be cleaner — flagged as a small follow-up.

Per host sim tick:
1. `worldShared.step(simDt)` advances all rocket bodies.
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

### Hitscan path (NOT changed in M20)

The hitscan rifle still uses `engine/math/Ray.h` + `intersectRayAabb`
against arena AABBs inside `resolveHitscanHost`. The world-vs-player
comparison was already in place before M20 (the rifle has never shot
through walls — a misread in the M20 spec). Migrating hitscan onto
Jolt raycast would only matter once dynamic bodies (ragdolls, etc.)
need to be hit by bullets, since the arena is a small static set
where the engine's Ray math is cheaper than Jolt's broadphase.

### Retirement

`engine/game/ProjectileSim.{h,cpp}` and `tests/test_projectile_sim.cpp`
are gone. The `iron::Projectile` POD relocated to
`games/07-net-shooter/Projectile.h` as `iron::netshooter::Projectile`
— still used by client-side ghost rendering and the
`SpawnProjectileMsg` payload. Host-side rocket tracking now lives in
the `HostRocket` struct in main.cpp (body + projectileId + ownerPeerId
+ spawnTimeSec). `RocketLauncher.cpp` lost `spawnRocketHost`,
`tickRocketHost`, `RocketSpawn`, and `RocketTickResult` — the host's
rocket logic now lives inline in net-shooter's main.cpp.

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
- Future: hitscan-on-Jolt swap, once ragdolls/dynamic targets need to
  be hit (currently engine `Ray` math against arena AABBs is sufficient).

## See also

- Upstream Jolt docs: <https://jrouwe.github.io/JoltPhysics/>
- vcpkg port: `joltphysics` (declared in the root `vcpkg.json`)
- M18 spec: `docs/superpowers/specs/2026-05-27-m18-jolt-physics-ragdoll-playground-design.md`
- M18 plan: `docs/superpowers/plans/2026-05-27-m18-jolt-physics-ragdoll-playground.md`
- [[networking]] — the prediction / lag-compensation layer that consumes
  the determinism guarantee.
