# M19 — Capsule Character Controller (Design Spec)

**Date:** 2026-05-27
**Milestone:** M19 (second of the physics overhaul track, after M18)
**Status:** Design — awaiting implementation plan

## Goal

Bring a real first-person capsule character controller to the engine on top of Jolt's `JPH::CharacterVirtual`. Port `games/07-net-shooter`'s WASD movement off the current no-collision point-clamp simulate onto the new controller. After M19 lands, players in net-shooter have wall/floor collision, gravity, ground detection, and a working jump — and the engine has the reusable controller other games can adopt.

## Direction context

This is the second milestone in the M18-M21 physics overhaul track. M18 brought Jolt in and validated rigid bodies + joints + ragdolls in `games/09-physics-playground`. The current net-shooter still uses M8.6's `simulate(state, input, dt)` lambda that just adds dx/dz deltas and clamps `y >= 0` — players walk through walls and can't jump. M19 replaces that simulate with a Jolt-driven one while preserving the M8.5 networked-prediction architecture (server-authoritative `PredictionEngine` with client prediction + reconciliation).

The TF2/Overwatch-class target game scope requires capsule character controllers that match player expectations: solid walls, slopes, stairs, jumps, gravity. M19 builds that foundation.

## Non-Goals

- **Player-vs-player collision.** Each player gets their own `PhysicsWorld` for movement determinism; players still see each other as networked positions but cannot push each other or stand on each other's heads. This unlocks in a later milestone if gameplay needs it.
- **Crouch, sprint, slide, mantle.** Standard capsule movement only. Movement modes are a TF2/Overwatch-feel polish track, not M19.
- **Ragdolls when a player dies.** That's M21.
- **Projectiles + raycasts on Jolt.** That's M20. Net-shooter rockets keep using `engine/game/ProjectileSim`; hitscan rifle keeps using `engine/math/Ray` raycasts vs the arena's game-side AABBs.
- **Per-axis movement input on the wire.** Wire format already separates dx/dy/dz on `PlayerInputMsg`; we just add a `jump` bit.
- **Ragdoll character mesh.** Bones in `games/09-physics-playground` are colored cubes; the player in net-shooter stays as the existing simple cube mesh. Skeletal-animated character rendering is its own future track.

## Architecture

### 1. `engine/physics/CharacterController.h/.cpp`

New engine wrapper around `JPH::CharacterVirtual`. One instance per simulated character. Owns no state outside the underlying Jolt object + a small config struct.

```cpp
namespace iron {

struct CharacterControllerConfig {
    float radius        = 0.30f;
    float halfHeight    = 0.90f;       // capsule body (excluding hemispheres);
                                        // total capsule height = 2 * (halfHeight + radius) = 2.4m
    float maxSlopeRad   = 45.0f * (3.14159f / 180.0f);
    float stepHeight    = 0.30f;       // max step climbable without jump
    float jumpVelocity  = 5.5f;        // m/s upward on jump (~1.5m peak)
    float gravity       = -9.81f;      // applied per update along world up
};

class CharacterController {
public:
    CharacterController();
    ~CharacterController();

    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    bool create(PhysicsWorld& world, const CharacterControllerConfig& cfg,
                Vec3 footPosition);
    void destroy(PhysicsWorld& world);

    // Per-tick: applies horizontal desiredVelocity (x, z) directly; applies
    // gravity to vertical velocity; if `wantJump` AND `isGrounded`, sets
    // vertical velocity to `cfg.jumpVelocity`. Steps the wrapped Jolt
    // character against the world's static (and other dynamic) geometry.
    // Caller MUST also call PhysicsWorld::step(dt) — character.update only
    // moves THIS character; the rest of the world advances via world.step.
    void update(float dt, Vec3 desiredVelocity, bool wantJump);

    // State accessors.
    Vec3  footPosition() const;     // bottom of capsule
    Vec3  velocity()     const;
    bool  isGrounded()   const;

    // Direct state mutators (used by reconciliation replay in PredictionEngine).
    void setFootPosition(Vec3);
    void setVelocity(Vec3);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
```

**Pimpl rationale matches `iron::PhysicsWorld`** — the Jolt header pulls in heavy SIMD math; pimpl keeps build times stable in game-side code.

### 2. Per-player PhysicsWorld topology

| Side | Worlds | Characters |
| ---- | ------ | ---------- |
| Client | 1 (the local player) | 1 |
| Host  | N (one per peer including self) | N |

Each world contains:
- The static arena geometry (walls + floor)
- Exactly one `CharacterController`

This isolation means each simulate call is deterministic — no cross-player interference. Players do not collide with each other in M19. Hitscan and rockets still resolve player hits via the existing `LagCompensator` (game-side AABB sweep), unchanged from M8.6.

### 3. Net-shooter `simulate` rewrite

Extend `PlayerState` from 3 floats (position only) to 7 fields (position + velocity + grounded). Extend `PlayerInput` from 3 floats (delta in world-space) to 3 fields (desired velocity in world-space x/z + jump bit).

```cpp
struct PlayerState {
    float x, y, z;       // foot position (capsule bottom)
    float vx, vy, vz;    // velocity (m/s)
    bool  grounded;
};

struct PlayerInput {
    float vx, vz;        // desired horizontal velocity (world-space, pre-rotated by yaw)
    bool  jump;          // edge-detected; engine clears after applying
};

// Capture controller + world by reference. Each player (client local, or
// host's per-peer simulate lambda) has its own controller + world.
auto simulate = [&world, &controller](PlayerState s, PlayerInput in, float dt) {
    controller.setFootPosition({s.x, s.y, s.z});
    controller.setVelocity({s.vx, s.vy, s.vz});
    controller.update(dt, {in.vx, 0.0f, in.vz}, in.jump);
    world.step(dt);  // advances static-world physics (currently no other dynamics)

    const Vec3 p = controller.footPosition();
    const Vec3 v = controller.velocity();
    return PlayerState{p.x, p.y, p.z, v.x, v.y, v.z, controller.isGrounded()};
};
```

The lambda is still pure-function relative to `(state, input, dt)` — it sets controller state at entry, reads it back at exit. Reconciliation replay (calling simulate N times in a row to catch up after an authoritative correction) works because each call resets controller state at entry.

### 4. Net-shooter input collection

Replace the `kMoveSpeed * (camera-relative axis)` scaling that today produces `PlayerInput{dx, dy, dz}`:

```cpp
// Today (M8.6):
input.dx = moveForward * cosYaw + moveStrafe * sinYaw;
input.dz = ...

// M19:
const float kMoveSpeed = 6.0f;
PlayerInput in;
in.vx   = (forwardAxis * cosYaw + strafeAxis * sinYaw) * kMoveSpeed;
in.vz   = (forwardAxis * sinYaw - strafeAxis * cosYaw) * kMoveSpeed * -1.0f;
                                                                // sign matches existing convention
in.jump = input.keyPressed(GLFW_KEY_SPACE);  // edge-detected, NOT keyDown
```

(Edge-detection ensures one jump per press, not continuous jump while held.)

### 5. Static arena ingestion

`Arena` (in `games/07-net-shooter/Arena.h`) already exposes `std::vector<Wall> walls` where each `Wall` has a center and half-extents (used by hitscan + visuals). At net-shooter startup, for each `PhysicsWorld` the game creates:

```cpp
void populateArenaCollision(iron::PhysicsWorld& world,
                            const iron::netshooter::Arena& arena) {
    // Floor: thin static box, 2 m below ground level center so its top edge is at y=0.
    world.createStaticBox({0.0f, -0.5f, 0.0f},
                          {arena.halfWidth, 0.5f, arena.halfDepth});
    for (const auto& wall : arena.walls) {
        world.createStaticBox(wall.center, wall.halfExtents);
    }
}
```

Game-side helper (not engine — the engine doesn't know about `netshooter::Arena`). Lives next to net-shooter's other game-side helpers.

### 6. Wire format changes

`games/07-net-shooter/Messages.h`:

```cpp
// PlayerInputMsg grows from 16 → 12 bytes (we replace dy with jump flag).
// Actually let's add rather than replace for explicitness; future M-tracks
// may want full 3D input (e.g. for swimming). Keep dy as 0 in v1.

struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float vx;
    float vy;        // unused in v1, reserved
    float vz;
    std::uint8_t jump;  // 0 or 1
};
static_assert(sizeof(PlayerInputMsg) == 4 + 4*3 + 1, "...");  // 17 bytes raw

// AuthorityPositionMsg grows to include velocity + grounded.
struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint8_t peerId;
    float x, y, z;
    float vx, vy, vz;
    std::uint8_t grounded;
    std::uint32_t lastInputId;
};
```

Both grow modestly. Game ID bumps so old clients can't connect: net-shooter's `kGameId` in `Messages.h` changes to a new magic value (any change suffices — the M8.4 handshake validates exact equality).

### 7. Camera + visual rendering

The camera continues to render the player's eyes at `footPosition + (0, kEyeHeight, 0)` where `kEyeHeight = 1.6f`. The character's capsule has total height `2 * (halfHeight + radius) = 2.4m`, so the eyes sit at 1.6m and the top of the head is at 2.4m — slight gap (the capsule is "tall enough that the eye position has hair on top"). Realistic and uncrowded.

The on-screen player avatar (when other players see the local player) remains a colored cube of M8.6-era dimensions. Capsule rendering is a future polish.

### 8. Determinism contract

The new simulate must remain byte-deterministic across client and host. `iron::PhysicsWorld` (from M18) is already configured for deterministic single-threaded simulation. `JPH::CharacterVirtual` is itself deterministic given the same world state. Per-player worlds means no cross-player non-determinism.

Reconciliation in `PredictionEngine`:
1. AuthorityPositionMsg arrives with `(pos, vel, grounded, lastInputId)`.
2. Predictor's `reconcile(authState, lastConfirmedInputId)` snaps `predicted_ = authState`, then replays inputs from history.
3. Each replay step calls `simulate(predicted_, replayInput, fixedDt)`.
4. Inside simulate, controller is reset to predicted_, advanced, read back.

Crucially: between reconciliation replay steps, the local `PhysicsWorld` advances by `fixedDt * replayInputCount`. The world contains only the static arena (no other dynamics affected by time), so this is correct.

## Tasks

Three subagent-friendly chunks. Estimated wall-clock: ~1 day each (Tasks 1 and 2 are the bulk; Task 3 is fast).

1. **`iron::CharacterController` engine API + tests** — create header + impl + 4 unit tests: collision against a static wall (walking into it stops), gravity (falls to ground over 1s ≈ -4.9m drop), jump (vertical velocity becomes `cfg.jumpVelocity` on jump from grounded), step climb (auto-steps over a 0.25m static box).

2. **Net-shooter port** — extend `PlayerState`/`PlayerInput`/`PlayerInputMsg`/`AuthorityPositionMsg`; populate arena geometry into per-player `PhysicsWorld`s at start; replace the simulate lambda; wire `Space` key for edge-detected jump; bump `kGameId`.

3. **Docs** — append M19 section to `docs/engine/physics.md` (`iron::CharacterController` API, per-player physics worlds rationale, the determinism + reconciliation contract).

## Tests

`tests/test_character_controller.cpp`:
- **Collision**: Spawn character at origin; static box wall 1m in front. Push character forward; after 1s it should have moved less than 1m (blocked by wall).
- **Gravity**: Spawn character at y=10. Step world for 1s. Foot position dropped ≈ 4.9m (½gt²).
- **Jump**: Spawn character grounded. `update(dt, {0,0,0}, jump=true)`. Velocity y becomes `cfg.jumpVelocity`. After one tick, foot rises.
- **Step climb**: Spawn character on floor next to a 0.25m static box. Push forward. Foot ends up on top of the box (auto-step) — y > 0.20.
- **Determinism**: Two `(world, controller)` pairs with identical inputs over 120 steps produce identical foot positions. Byte-equal (matches the M18 `PhysicsWorld` determinism test pattern).

Integration verification (manual, in net-shooter):
- Walk into the arena walls — character stops (cannot walk through)
- Walk off a ledge — character falls and lands
- Press space — character jumps
- Run + jump simultaneously — controller respects horizontal momentum while airborne
- Standing on the floor + not pressing keys — character stays at rest (no jitter)
- Networked play (host + client on same machine): client prediction is smooth; server reconciliation does not visibly snap during normal play

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| `JPH::CharacterVirtual::ExtendedUpdate` requires the wrapped `JPH::CharacterVirtual` to be in the same `JPH::PhysicsSystem` as the static geometry it collides with | The controller's `create()` takes `PhysicsWorld&` and reaches into its `Impl` to register the character with the system. Pimpl forward-decl already supports this from M18. |
| Per-player PhysicsWorlds on the host = N+1 PhysicsSystem instances on the host. Memory? | Each `JPH::PhysicsSystem` is a few MB. With ≤8 players that's modest. Future optimization: single world + shared static geometry + player layer that ignores other player capsules. |
| `JPH::CharacterVirtual` has subtle ground-detection quirks (e.g., "edge of platform" doesn't ground) | Test against simple flat floor at first; tune `cMaxNumHits` and other char-virtual params via Jolt's defaults; iterate visually. |
| `PredictionEngine` replay touches the world N times per reconciliation. If the local world has dynamics outside the character, those advance too | M19 worlds contain only static arena + 1 character; no other dynamics. Safe for v1. Document the assumption; M20 adds projectiles which probably need their own world or careful state restore. |
| Wire format break = old clients can't connect | Bump `kGameId`. Add a friendly mismatch message in the existing handshake (already logs on mismatch from M8.4). |
| `JPH::CharacterVirtual` requires a `CharacterVirtualSettings` shape — capsule, mass center, supporting volume | Capsule via `JPH::CapsuleShape(halfHeight, radius)`. Mass center at capsule center. Settings ship with sensible defaults. |
| Step-up adds visual judder when running at small obstacles | Acceptable v1; can be smoothed with eye-position lerp in a future polish pass |

## Verification

- **CI green** — new tests pass; existing tests don't regress.
- **Net-shooter solo run**: walk + collide with walls + jump + gravity all work; FPS still 60+.
- **Net-shooter co-op (host + client)**: prediction smooth, reconciliation invisible during normal play, no de-syncs.
- **Hitscan + rockets** still work (they don't depend on the movement system).

## Follow-ups (not in M19)

- **M20** — projectile rigid bodies + raycasts in net-shooter.
- **M21** — death-into-ragdoll wiring.
- Player-vs-player physics collision (post-M21, when needed).
- Per-character capsule visualization in 3rd-person view.
- Movement polish (crouch, sprint, slide, wall-jump, mantling) as its own track.
- Skeletal animation track — render skinned characters instead of cubes once the asset pipeline arrives.
