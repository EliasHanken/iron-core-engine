# M18 — Jolt Physics Integration + Ragdoll Playground (Design Spec)

**Date:** 2026-05-27
**Milestone:** M18 (first of the physics overhaul track)
**Status:** Design — awaiting implementation plan

## Goal

Bring Jolt Physics into the engine and ship a visual validator that demonstrates rigid bodies, joints, and a multi-body ragdoll. After M18 lands, the engine has a production-grade physics foundation and a playground scene where ragdolls tumble down ramps, knock over box stacks, and get launched by player-fired spheres.

This is the first milestone of a 4-milestone physics overhaul track:
- **M18 (this spec):** Jolt integration + ragdoll playground
- **M19:** Capsule character controller (port net-shooter movement onto Jolt)
- **M20:** Projectile rigid bodies + raycasts (port net-shooter rockets + hitscan)
- **M21:** Death-into-ragdoll wiring in net-shooter

## Direction context

The engine direction has shifted: target game scope is now TF2 / Overwatch-style class-based 3D multiplayer shooters. This requires real physics — capsule character controllers, projectile rigid bodies, raycasts that interact with dynamic bodies, ragdolls on death. Building this from scratch would take 4-6 dedicated milestones; using Jolt collapses that to 2-3 milestones of integration and game-wiring work.

**Why Jolt over PhysX / Bullet:** clean modern C++17 API, deterministic by default (critical for the engine's server-authoritative networking with client prediction — M8.5 PeerManager + PredictionEngine), fast compile times, small static lib, excellent character controller. PhysX's wins (GPU acceleration for cloth/fluids/destruction, PVD debugger, industry recognition) don't outweigh the iteration-speed cost for an indie engine. Selected after explicit comparison (see chat 2026-05-27).

## Non-Goals

- Networking ragdolls (single-process playground demo only).
- Skeletal animation (each ragdoll bone renders as a colored cube — no skinning, no bone hierarchy at the render layer).
- Cloth, soft bodies, vehicles, fluids — Jolt doesn't ship these, and we don't need them.
- Replacing the existing Verlet rope physics (M3) — frozen alongside OpenGL, only used in `02-strandbound`.
- Porting net-shooter to Jolt — that's M19/M20/M21.
- A physics editor UI — playground uses keyboard controls.

## Architecture

### `engine/physics/PhysicsWorld.h/.cpp`

Thin wrapper around Jolt's `PhysicsSystem`. Owns the system, body interface, temp allocator, job system, and broadphase/object layer filters. Exposes opaque engine-typed handles (`BodyId`, `JointId`) wrapping Jolt's internal IDs. **Game code never includes a Jolt header** — wrapper is the only public surface.

```cpp
namespace iron {

// Opaque handle types. Game code stores these but does not inspect them.
struct BodyId  { uint32_t value = 0; };
struct JointId { uint32_t value = 0; };

constexpr BodyId  kInvalidBody  {};
constexpr JointId kInvalidJoint {};

class PhysicsWorld {
public:
    bool init();
    void shutdown();

    // Deterministic fixed-step. Caller picks dt (typically 1/60).
    void step(float dt);

    // --- Body creation ---
    BodyId createStaticBox     (Vec3 pos, Vec3 halfExtents);
    BodyId createStaticPlane   (Vec3 pos, Vec3 halfExtents);     // sugar for thin static box
    BodyId createDynamicBox    (Vec3 pos, Vec3 halfExtents, float mass);
    BodyId createDynamicSphere (Vec3 pos, float radius,         float mass);
    BodyId createDynamicCapsule(Vec3 pos, float halfH, float r, float mass);
    void   destroyBody(BodyId);

    // --- Body state ---
    Vec3       bodyPosition(BodyId)  const;
    Quaternion bodyRotation(BodyId)  const;
    Mat4       bodyTransform(BodyId) const;   // convenience for renderer
    bool       isBodyAlive(BodyId)   const;

    // --- Forces / impulses ---
    void applyImpulse       (BodyId, Vec3 impulse);
    void applyForceAtPoint  (BodyId, Vec3 force, Vec3 worldPoint);
    void setVelocity        (BodyId, Vec3 vel);

    // --- Queries ---
    struct RaycastHit {
        bool   hit       = false;
        BodyId body       = kInvalidBody;
        Vec3   point      {};
        Vec3   normal     {};
        float  t          = 0.0f;        // 0..1 along the ray
    };
    RaycastHit raycast(Vec3 origin, Vec3 direction, float maxDistance) const;

    // --- Joints ---
    // Swing-twist: spine, neck, shoulders, hips. World-space pivot + twist axis.
    JointId createSwingTwistJoint(BodyId a, BodyId b,
                                  Vec3 pivotWorld, Vec3 twistAxisWorld,
                                  float swingLimitRad, float twistLimitRad);
    // Hinge: elbow, knee. World-space pivot + hinge axis + signed angle limits.
    JointId createHingeJoint(BodyId a, BodyId b,
                             Vec3 pivotWorld, Vec3 hingeAxisWorld,
                             float minAngleRad, float maxAngleRad);
    void destroyJoint(JointId);

private:
    // Pimpl: hide Jolt types so game code never includes JPH/.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
```

**Design choices:**

- **Pimpl idiom** keeps Jolt out of the engine's public headers. The wrapper header includes only `<memory>` and the engine's math headers.
- **`Vec3` / `Quaternion` / `Mat4`** are the engine's existing math types. Wrapper converts to/from `JPH::Vec3` / `JPH::Quat` / `JPH::Mat44` inside the impl.
- **`BodyId` / `JointId` as 32-bit POD** matches the engine's existing handle convention (`MeshHandle`, `TextureHandle`, etc.).
- **Single physics world per `iron::Renderer`-equivalent** — games construct one `PhysicsWorld` in `main()` and pass it around.

### `engine/physics/Ragdoll.h/.cpp`

Declarative humanoid skeleton built on `PhysicsWorld`. Knows how to spawn 11 bodies + 10 joints at a position and report bone transforms for rendering.

```cpp
namespace iron {

struct RagdollSpec {
    float totalHeight = 1.8f;    // overall height; all bone dims scale from this
    float mass        = 75.0f;   // total mass distributed across bones
};

class Ragdoll {
public:
    static constexpr int kBoneCount = 11;

    // Spawns 11 dynamic bodies + 10 joints. The hips body is at `position`.
    // `rotation` rotates the whole skeleton in world space.
    void spawn(PhysicsWorld&, const RagdollSpec&, Vec3 position, Quaternion rotation);

    // Destroys all bodies + joints. Safe to call multiple times.
    void despawn(PhysicsWorld&);

    // Per-bone access for the renderer.
    int  boneCount()                const { return kBoneCount; }
    Mat4 boneTransform(int idx)     const;        // world-space, updated every step
    Vec3 boneHalfExtents(int idx)   const;        // local half-extents for the cube renderer
    Vec3 boneColor(int idx)         const;        // distinct color per bone (head red, etc.)

    bool isSpawned() const { return bones_[0].value != 0; }

private:
    std::array<BodyId,  kBoneCount> bones_  {};
    std::array<JointId, 10>         joints_ {};
    std::array<Vec3,    kBoneCount> halfExtents_ {};  // cached for renderer
    PhysicsWorld*                   world_  = nullptr;
};

}  // namespace iron
```

### Ragdoll skeleton layout (11 bones, 10 joints)

Bone indices, shapes, and approximate dimensions for a 1.8m human:

| Idx | Bone        | Shape | Half-extents (1.8m)      | Mass frac |
| --: | ----------- | :---: | ------------------------ | --------: |
|   0 | head        | box   | 0.10 × 0.125 × 0.10      |    0.08   |
|   1 | torso       | box   | 0.18 × 0.25  × 0.11      |    0.36   |
|   2 | hips        | box   | 0.18 × 0.09  × 0.11      |    0.14   |
|   3 | upper arm L | box   | 0.05 × 0.15  × 0.05      |    0.03   |
|   4 | forearm L   | box   | 0.04 × 0.16  × 0.04      |    0.025  |
|   5 | upper arm R | box   | 0.05 × 0.15  × 0.05      |    0.03   |
|   6 | forearm R   | box   | 0.04 × 0.16  × 0.04      |    0.025  |
|   7 | upper leg L | box   | 0.065 × 0.21 × 0.065     |    0.09   |
|   8 | lower leg L | box   | 0.05 × 0.21  × 0.05      |    0.06   |
|   9 | upper leg R | box   | 0.065 × 0.21 × 0.065     |    0.09   |
|  10 | lower leg R | box   | 0.05 × 0.21  × 0.05      |    0.06   |

Total mass fraction ≈ 1.0. All dimensions scale linearly with `RagdollSpec::totalHeight`. Mass scales with `RagdollSpec::mass`.

Joint setup (pivots in world space at spawn time):

| Joint | Bodies | Type | Limits |
| :---: | :---: | :---: | :---: |
| Spine | torso ↔ hips | swing-twist | swing ±25°, twist ±15° |
| Neck | head ↔ torso | swing-twist | swing ±40°, twist ±60° |
| Shoulder L | upperArmL ↔ torso | swing-twist | swing ±90°, twist ±45° |
| Shoulder R | upperArmR ↔ torso | swing-twist | swing ±90°, twist ±45° |
| Elbow L | forearmL ↔ upperArmL | hinge | 0° to 145° |
| Elbow R | forearmR ↔ upperArmR | hinge | 0° to 145° |
| Hip L | upperLegL ↔ hips | swing-twist | swing ±60°, twist ±30° |
| Hip R | upperLegR ↔ hips | swing-twist | swing ±60°, twist ±30° |
| Knee L | lowerLegL ↔ upperLegL | hinge | -145° to 0° |
| Knee R | lowerLegR ↔ upperLegR | hinge | -145° to 0° |

### Playground demo (`games/09-physics-playground`)

**Scene:**
- 30m × 30m static ground plane (textured)
- Three static ramps at 15° / 30° / 45° side by side, ~3m long each, ~1m wide
- One low static wall (0.5m tall × 4m long) positioned to ricochet falling ragdolls
- A dynamic 4-box stack (each 0.5m³, 5kg) near the ramps
- Sunset cubemap skybox (existing M16 procedural sunset)

**Controls:**
- WASD + QE + mouse: free-fly camera (existing `iron::FreeFlyCamera`)
- `R`: spawn a ragdoll 3m in front of the camera, with a small upward velocity (~2 m/s) and a random rotation so it tumbles in a different way each time
- `B`: fire a 5 kg sphere (0.3 m radius) from the camera at 30 m/s
- `C`: clear all ragdolls and reset the dynamic box stack
- `F3`: toggle physics debug gizmos (body AABBs, joint pivots) — uses existing `GizmoRegistry`
- `ESC`: quit

**HUD (existing M11 HUD subsystem):**
- Top-left: FPS, active body count, ragdoll count
- Bottom-left: key hints (`R: spawn ragdoll`, `B: shoot ball`, `C: clear`, `F3: gizmos`)

**Rendering:**
- Each rigid body draws as a single cube via the existing scene-mesh API + the existing Vulkan lit shader. Per-frame, the game pulls `bodyTransform(BodyId)` and uses it as the draw's `model` matrix.
- Ragdoll bones use `Ragdoll::boneColor(idx)` for distinct per-bone hues (head red, torso blue, limbs varied) — pre-skeletal-animation, distinct colors make rotation legible at a glance.
- Dynamic stack boxes are uniform brown; the player-fired ball is bright yellow; ramps + wall use the existing CC0 ground/brick textures.

### Frame flow

```
beginFrame()              -- existing renderer
poll input (R / B / C)
if (R) spawnRagdolloon()
if (B) fireBall()
if (C) clearAll()
physicsWorld.step(dt)     -- deterministic, dt = frame time clamped to [1/120, 1/30]
for each body in scene:
    renderer.submit(call with model = physicsWorld.bodyTransform(body))
flushHud()
flushDebugGizmos() if F3
endFrame()
```

The fixed-vs-variable timestep question is real but for v1 we use frame-time dt clamped to a sane range (1/120 to 1/30 s). A proper accumulator (`FixedTickScheduler` from M8.4) can come in M19 when networking enters the picture.

## Build integration

- **vcpkg dependency:** add `"joltphysics"` to `vcpkg.json`.
- **Top-level CMake:** `find_package(unofficial-joltphysics CONFIG REQUIRED)` (vcpkg port convention).
- **`engine/CMakeLists.txt`:** add `physics/PhysicsWorld.cpp` and `physics/Ragdoll.cpp` to `ironcore`. Link `unofficial::joltphysics::Jolt` **PRIVATE** so game code linking `ironcore` doesn't transitively pull Jolt headers.
- **`games/09-physics-playground/CMakeLists.txt`:** new directory. Links `ironcore` only — never Jolt directly.
- **CI:** vcpkg cache key auto-invalidates from `vcpkg.json` hash change. First cold build will compile Jolt (~3 min); subsequent CI runs use the cached binary.

## Testing

### `tests/test_physics_world.cpp`
- Init + shutdown does not leak.
- Create a body, query position, destroy. Position matches what was set.
- Step deterministic: run two `PhysicsWorld` instances side-by-side with identical setup, step both N times, assert bodies are at identical positions (byte-equal).
- Gravity: drop a dynamic body, after 1 second it should have fallen ~4.9 m (within rounding).
- Raycast: aim at a known static box, verify hit position + normal.
- Impulse: apply a known impulse, verify resulting velocity from `m * v = J`.

### `tests/test_ragdoll.cpp`
- `Ragdoll::spawn` creates `kBoneCount` bodies and 10 joints.
- All ragdoll bodies are dynamic (not static).
- `despawn` removes all bodies and joints (body count returns to pre-spawn baseline).
- After spawning at position P, the hips body is at P.
- After stepping 2 seconds with no obstacles below, the ragdoll has fallen ~19.6 m (free-fall).

### Visual validation (no automated test)
- Run `.\build-vk\games\09-physics-playground\Debug\physics-playground.exe`
- Press `R` repeatedly — ragdolls spawn, tumble, land on the ground with believable poses (not floppy-noodle, not stick-figure-rigid)
- Spawn ragdolls onto the ramps — they slide and tumble down
- Fire balls at the box stack — boxes scatter
- Fire balls at ragdolls — they react, limbs flail proportionally to hit force
- Press `C` — everything clears, FPS recovers

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| `joltphysics` vcpkg port quality | Port is stable as of 2026; if issues, fall back to git submodule. CI catches first. |
| Joint tuning takes longer than estimated (ragdolls floppy or stiff) | Time-box: 1 day on joint params. If not right, lock current values and adjust in a fixup. |
| Mass/inertia balance feels wrong (head too heavy, limbs too light) | Validate visually; iterate. Default values come from biomechanics tables — usually close. |
| Determinism test flakes (Jolt config not fully deterministic by default) | Jolt requires `mUseLargeIslandSplitter = false` and a single thread for byte-determinism. Document + enforce in `PhysicsWorld::init`. |
| Compile time of Jolt under MSVC | Acceptable: Jolt is a single static lib, ~5-15 s clean. Header inclusion is contained via pimpl. |
| Memory leaks from forgotten body destruction | `PhysicsWorld::shutdown` destroys all bodies. `Ragdoll::despawn` is idempotent. |
| Rendering 50 ragdolls = 550 draw calls | Acceptable for v1. Future milestone could batch via instancing if it becomes hot. |
| F3 debug gizmos require pulling body AABBs out of Jolt | `JPH::Body::GetWorldSpaceBounds()` exposes the AABB. Wrapper helper added. |

## Tasks

Four subagent-friendly chunks:

1. **vcpkg + `PhysicsWorld` wrapper** — Jolt enters the build via vcpkg manifest; `iron::PhysicsWorld` inits/shuts down; basic body create/destroy/step works; raycast + applyImpulse work; tests in `test_physics_world.cpp` pass. Standalone — no game uses it yet.

2. **Joints + `Ragdoll` class** — swing-twist and hinge joint wrappers; `Ragdoll::spawn` creates the 11-body skeleton with 10 joints anchored at the correct world-space pivots; bone half-extents + colors exposed; `despawn` cleans up; tests in `test_ragdoll.cpp` pass. Standalone.

3. **`games/09-physics-playground` demo** — new game directory; scene setup (ground + ramps + wall + dynamic box stack + skybox); R/B/C controls; HUD with body count + key hints; free-fly camera; F3 debug gizmos pulling body AABBs from physics; renders each body as a colored cube.

4. **Docs** — new `docs/engine/physics.md` covering: PhysicsWorld API surface, BodyId / JointId handle pattern, Ragdoll skeleton + joint layout, the determinism contract for future networking, vcpkg integration notes, link to Jolt's upstream docs.

## Verification

- **CI green** on Windows MSVC build (new game target + new tests).
- **Visual:** `.\build-vk\games\09-physics-playground\Debug\physics-playground.exe` shows ragdolls tumbling down ramps and reacting to fired balls. Stack of boxes can be knocked over.
- **Determinism test passes** — two physics worlds with identical inputs produce byte-identical body states after N steps.

## Follow-ups (NOT in M18)

- **M19** — `iron::CharacterController` capsule controller, port net-shooter movement onto Jolt.
- **M20** — Net-shooter rockets → Jolt rigid bodies; hitscan → Jolt raycasts.
- **M21** — Net-shooter death-into-ragdoll wiring (uses M18 machinery).
- Skeletal animation track (separate from physics) — needed before ragdolls can drive skinned meshes.
- Fixed-tick accumulator integration with `FixedTickScheduler` (M8.4) once networking enters in M19.
- Networked ragdoll replication — not a v1 concern.
- glTF + skeletal animation asset pipeline — separate future track.
