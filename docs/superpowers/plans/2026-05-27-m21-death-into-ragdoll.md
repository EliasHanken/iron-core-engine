# M21 Death-into-Ragdoll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When a net-shooter player dies, replace the instant-disappear corpse with a tumbling 11-body `iron::Ragdoll` spawned at the death position with an impulse on the torso (hitscan ray direction or rocket explosion direction). Ragdoll despawns at 2-second respawn timer.

**Architecture:** Every peer (host + clients) maintains its own `iron::PhysicsWorld worldShared` containing arena collision; host's also holds rockets from M20. New `DeathMsg` wire packet broadcast by host at every kill site includes the death position + a pre-computed torso impulse. Each peer (host loopback + client via handler) spawns a `iron::Ragdoll` locally and applies the impulse. Render loop skips the player cube when their peerId is in `activeRagdolls`, and a parallel loop renders each ragdoll's 11 bones as colored cubes via the existing emissive-tint pattern (matches how rockets, tracers, explosions are tinted today).

**Tech Stack:** C++23, Jolt Physics (via M18 `iron::PhysicsWorld` + M18 `iron::Ragdoll`), GameNetworkingSockets, CMake, MSVC.

---

## File Structure

### Modified
- `games/07-net-shooter/Messages.h` — add `DeathMsg`, bump `kGameId`
- `games/07-net-shooter/main.cpp` — all the wiring (per-peer `worldShared` for clients, `spawnLocalRagdoll` helper, `DeathMsg` handler, host-side broadcast at 2 kill sites, per-frame step + despawn, render integration, cleanup on `onPeerLeft`)
- `docs/engine/physics.md` — append M21 section

### No new files; no engine changes
M18's `iron::Ragdoll` + M18's `iron::PhysicsWorld::applyImpulse` are exercised; no engine API additions needed.

---

## Task 1: `DeathMsg` wire + per-peer `worldShared` on clients

**Files:**
- Modify: `games/07-net-shooter/Messages.h`
- Modify: `games/07-net-shooter/main.cpp` (only the client-side world setup; no death-handling yet)

After this task, the wire message exists and every peer initializes a `worldShared`. No game-visible behavior change yet — Task 2 wires the death handling. Build is clean.

- [ ] **Step 1: Add `DeathMsg` + bump `kGameId` in `Messages.h`**

Open `games/07-net-shooter/Messages.h`. Make two edits:

```cpp
// "NSTT" — net shooter. Bumped for M21 (added DeathMsg, wire-format change).
// Old clients cannot connect to new hosts.
constexpr std::uint32_t kGameId = 0x4E535454u;  // was 0x4E535453u (NSTS, M19)
```

At the bottom of the file, before the closing `}  // namespace iron::netshooter`, add:

```cpp
// Host -> Clients: broadcast when a player dies (HP -> 0). Carries the
// death foot position + a pre-computed impulse vector for the ragdoll
// torso. Each peer spawns its own ragdoll on receipt; no per-tick
// bone-transform sync.
struct DeathMsg {
    static constexpr std::uint8_t kTag = 11;
    std::uint32_t victimPeerId;
    float x, y, z;
    float impulseX, impulseY, impulseZ;
};
```

(Tag 11 — next free after `ScoreUpdateMsg` at tag 10. Verify by scanning the existing `kTag` values; if 11 is taken, use the next free integer in `[2, 253]`.)

- [ ] **Step 2: Register the new message type**

In `games/07-net-shooter/main.cpp`, find the `MessageRegistry registry;` setup block where the existing message types are registered (search for `registry.registerType<...>`). Add registration for the new type alongside the others:

```cpp
registry.registerType<iron::netshooter::DeathMsg>();
```

(If you can't find an explicit `registerType<...>` call — the engine's `MessageRegistry` might auto-register via the handler — skip this step. The grep should reveal which pattern is in use.)

- [ ] **Step 3: Create a `worldShared` on the client (host already has one from M20)**

Open `games/07-net-shooter/main.cpp`. Find the host-only declaration of `worldShared` from M20 (around line 776 — search for `iron::PhysicsWorld worldShared`). It's currently inside the host-only setup branch / scope.

**Move `worldShared` declaration UP** so both host and clients create one. The simplest path: declare `worldShared` at the same scope as `arena` (immediately after `arena` is built), so it's available to both branches:

```cpp
const iron::netshooter::Arena arena = iron::netshooter::buildArena(0xA5A5);

// M21 — every peer maintains its own worldShared. Host also adds rockets
// to it (M20). Clients only use it for ragdolls (M21). Each world contains
// the same arena collision so ragdolls fall correctly on every peer.
iron::PhysicsWorld worldShared;
worldShared.init();
populateArenaCollision(worldShared, arena);
```

If `worldShared.init()` + `populateArenaCollision(worldShared, arena)` already exist later in the host setup, remove those (the moved declaration above already does them once for everyone).

The M20 host-side `onContactStarted` callback registration + `hostRockets` + `pendingDespawns` setup should STAY in the host-only path — those are gameplay (rockets), not cosmetic (ragdolls). Verify by reading the surrounding code: the lambda capture of `hostRockets` only makes sense where `hostRockets` exists.

- [ ] **Step 4: Step `worldShared` on the client per frame**

The host already steps `worldShared` once per sim tick (M20, in the sim-tick block around line 1510). The client does NOT — so on the client side, step it in the render loop instead.

Find the client-side render loop. Just BEFORE the player-rendering block (around line 1715, right after `renderer.beginFrame(...)`), add:

```cpp
        // M21 — clients step worldShared each frame for ragdoll physics.
        // Host already steps it in the sim tick (see worldShared.step in
        // the sim block above).
        if (!peers.isHost()) {
            worldShared.step(static_cast<float>(frameDt));
        }
```

> If `frameDt` isn't the variable name in this scope, use whatever per-frame delta-time variable is already used (search for `deltaSec`, `dtFrame`, etc.). The render loop already has access to one. Clamp the dt if needed: `std::min(frameDt, 1.0f / 30.0f)`.

- [ ] **Step 5: Build**

```
cmake --build build-vk --config Debug --target net-shooter
```

Expected: clean build. The game behaves identically to M20 (no death-handling yet).

- [ ] **Step 6: Smoke test — basic connectivity still works**

Two PowerShell windows:

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --connect 127.0.0.1
```

Expected: both connect; players see each other; movement works; rockets work; hitscan works (everything from M19/M20). No corpse animation yet — that's Task 2.

If the client crashes with "PhysicsWorld::init failed" or similar: the client's worldShared init might be running before some other dependency. Move it later in client setup if needed.

- [ ] **Step 7: Commit**

```
git add games/07-net-shooter/Messages.h games/07-net-shooter/main.cpp
git commit -m "M21 Task 1: DeathMsg wire + per-peer worldShared on clients"
```

---

## Task 2: Spawn + render ragdoll on death (host broadcast + client receive)

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

The biggest task. After this, dying spawns a ragdoll on every peer, tumbles for 2 seconds, despawns.

- [ ] **Step 1: Add `ActiveRagdoll` + `activeRagdolls` state + `spawnLocalRagdoll` helper**

Open `games/07-net-shooter/main.cpp`. Find the existing `hostRockets` declaration (around line 786 from M20). **Immediately after** it (so both host + client see this — verify the scope is shared across both branches), add:

```cpp
    // M21 — Death ragdolls. Stored on every peer (host + clients) so
    // every peer renders the tumbling corpse locally. Despawned after 2s.
    struct ActiveRagdoll {
        std::uint32_t victimPeerId = 0;
        double        spawnTimeSec = 0.0;
        iron::Ragdoll ragdoll;
    };
    // unique_ptr so emplace doesn't require Ragdoll to be movable.
    std::unordered_map<std::uint32_t, std::unique_ptr<ActiveRagdoll>> activeRagdolls;

    auto spawnLocalRagdoll = [&](std::uint32_t pid, iron::Vec3 footPos,
                                  iron::Vec3 impulse) {
        // Despawn any existing ragdoll for this peer (edge case: two
        // fatal hits in the same tick).
        auto existing = activeRagdolls.find(pid);
        if (existing != activeRagdolls.end()) {
            existing->second->ragdoll.despawn(worldShared);
            activeRagdolls.erase(existing);
        }

        auto ar = std::make_unique<ActiveRagdoll>();
        ar->victimPeerId = pid;
        ar->spawnTimeSec = nowSec();
        ar->ragdoll.spawn(worldShared, iron::RagdollSpec{}, footPos);

        // Impulse on the torso bone — picks the satisfying direction
        // (hitscan: along ray; rocket: away from explosion).
        const iron::BodyId torso = ar->ragdoll.boneBody(iron::Ragdoll::kTorso);
        worldShared.applyImpulse(torso, impulse);

        activeRagdolls.emplace(pid, std::move(ar));
    };
```

- [ ] **Step 2: Host — compute + broadcast impulse at the hitscan kill site**

Find the hitscan `FireHitscanMsg` handler around line 1127-1146 (search for `iron::isAlive(vit->second.hp)` near the hitscan code). It currently does kill bookkeeping + score broadcasts. **At the end of that `if (!iron::isAlive(...))` block** (after the kill feed + score broadcast), append:

```cpp
                // M21 — compute hitscan death impulse along the ray direction.
                // msg.dx/dy/dz is the normalized aim direction (client sends
                // this in tryFireHitscanClient).
                constexpr float kHitscanImpulseMag = 30.0f;
                const iron::Vec3 impulse{
                    msg.dx * kHitscanImpulseMag,
                    msg.dy * kHitscanImpulseMag,
                    msg.dz * kHitscanImpulseMag,
                };
                const iron::Vec3 deathPos{
                    authStates[dm.victimPeerId].x,
                    authStates[dm.victimPeerId].y,
                    authStates[dm.victimPeerId].z,
                };
                peers.broadcastToAll<iron::netshooter::DeathMsg>(
                    iron::netshooter::DeathMsg{
                        dm.victimPeerId,
                        deathPos.x, deathPos.y, deathPos.z,
                        impulse.x, impulse.y, impulse.z,
                    },
                    iron::SendReliability::Reliable);
                spawnLocalRagdoll(dm.victimPeerId, deathPos, impulse);
```

- [ ] **Step 3: Host — compute + broadcast impulse at the rocket kill site**

Find the rocket despawn block around line 1567-1601 (the M20 `for (const auto& d : pendingDespawns)` loop, inside the splash damage application). Currently the `if (!iron::isAlive(vit->second.hp))` block does kill bookkeeping for splash kills. **At the end of that `if` block** (after the score broadcast), append:

```cpp
                            // M21 — compute rocket death impulse (away from
                            // explosion, biased upward).
                            const iron::Vec3 vpos{
                                authStates[pid].x,
                                authStates[pid].y,
                                authStates[pid].z,
                            };
                            iron::Vec3 fromExplosion{
                                vpos.x - d.point.x,
                                vpos.y - d.point.y + 1.0f,
                                vpos.z - d.point.z,
                            };
                            const float mag = std::sqrt(
                                fromExplosion.x*fromExplosion.x +
                                fromExplosion.y*fromExplosion.y +
                                fromExplosion.z*fromExplosion.z);
                            if (mag > 0.001f) {
                                fromExplosion.x /= mag;
                                fromExplosion.y /= mag;
                                fromExplosion.z /= mag;
                            } else {
                                fromExplosion = iron::Vec3{0.0f, 1.0f, 0.0f};
                            }
                            constexpr float kRocketImpulseMag = 50.0f;
                            const iron::Vec3 impulseR{
                                fromExplosion.x * kRocketImpulseMag,
                                fromExplosion.y * kRocketImpulseMag,
                                fromExplosion.z * kRocketImpulseMag,
                            };
                            peers.broadcastToAll<iron::netshooter::DeathMsg>(
                                iron::netshooter::DeathMsg{
                                    pid,
                                    vpos.x, vpos.y, vpos.z,
                                    impulseR.x, impulseR.y, impulseR.z,
                                },
                                iron::SendReliability::Reliable);
                            spawnLocalRagdoll(pid, vpos, impulseR);
```

> Verify `pid` is the victim peer id in this scope (it should be — the outer splash loop iterates `for (const auto pid : alivePeers)`). Verify `d.point` is the explosion point (it's the `pendingDespawns` entry's `point` from M20).

- [ ] **Step 4: Client `DeathMsg` handler**

In the client message registration block (alongside other `registry.registerHandler<...>` calls), add:

```cpp
    registry.registerHandler<iron::netshooter::DeathMsg>(
        [&](iron::ConnectionId, const iron::netshooter::DeathMsg& msg) {
            if (peers.isHost()) return;  // host already spawned locally
            spawnLocalRagdoll(
                msg.victimPeerId,
                iron::Vec3{msg.x, msg.y, msg.z},
                iron::Vec3{msg.impulseX, msg.impulseY, msg.impulseZ});

            // Also flip the hpForHud to 0 so the existing cube-hide guard
            // kicks in (DamageMsg may arrive in a different order).
            if (msg.victimPeerId == peers.myPeerId()) {
                localHpForHud = 0;
            } else {
                auto rit = remotes.find(msg.victimPeerId);
                if (rit != remotes.end()) {
                    rit->second.hpForHud = 0;
                }
            }
        });
```

- [ ] **Step 5: Per-frame despawn of 2s-old ragdolls (every peer)**

In the render loop, just AFTER the `worldShared.step` block from Task 1 Step 4, add:

```cpp
        // M21 — despawn ragdolls older than 2s (matches respawn timer).
        {
            const double now = nowSec();
            for (auto it = activeRagdolls.begin(); it != activeRagdolls.end(); ) {
                if (now - it->second->spawnTimeSec > 2.0) {
                    it->second->ragdoll.despawn(worldShared);
                    it = activeRagdolls.erase(it);
                } else {
                    ++it;
                }
            }
        }
```

- [ ] **Step 6: Skip the player cube when their ragdoll is active**

Find the player-rendering loops around line 1767-1788. They iterate `authStates` (host) and `remotes` (client) and call `submitPlayerCube(pid, pos)`.

In BOTH loops, add a guard just before the cube submission. For the host loop:

```cpp
        if (peers.isHost()) {
            for (const auto& [pid, state] : authStates) {
                if (pid == 0) continue;
                if (auto it = hostPlayers.find(pid);
                    it != hostPlayers.end() && it->second.respawnAtSec >= 0.0) {
                    continue;
                }
                // M21 — skip the player cube while their ragdoll is active.
                if (activeRagdolls.find(pid) != activeRagdolls.end()) continue;
                submitPlayerCube(pid, iron::Vec3{state.x, state.y, state.z});
            }
        } else {
            for (const auto& [pid, remote] : remotes) {
                if (pid == myId) continue;
                if (remote.hpForHud <= 0) continue;
                // M21 — skip the player cube while their ragdoll is active.
                if (activeRagdolls.find(pid) != activeRagdolls.end()) continue;
                auto pos = remote.positionHistory.sampleAtDelay(kDisplayDelay);
                if (!pos) continue;
                submitPlayerCube(pid, *pos);
            }
        }
```

- [ ] **Step 7: Render ragdoll bones (every peer)**

Immediately AFTER the player-cube rendering loop, add a new loop that renders ragdoll bones:

```cpp
        // M21 — render active ragdolls as colored bone cubes.
        for (const auto& [pid, ar] : activeRagdolls) {
            for (int i = 0; i < ar->ragdoll.boneCount(); ++i) {
                const iron::Mat4 model = ar->ragdoll.boneTransform(i);
                const iron::Vec3 he    = ar->ragdoll.boneHalfExtents(i);
                const iron::Vec3 color = ar->ragdoll.boneColor(i);

                iron::DrawCall call;
                call.mesh   = cubeMesh;
                call.shader = litShader;
                // boneTransform returns the bone's world matrix; multiply by
                // scaling(2*he) because cubeMesh is a unit cube.
                call.model  = model * iron::scaling(iron::Vec3{he.x * 2.0f,
                                                                 he.y * 2.0f,
                                                                 he.z * 2.0f});
                call.material.texture     = renderer.whiteTexture();
                call.material.normalMap   = renderer.flatNormalTexture();
                call.material.specularMap = renderer.noSpecularTexture();
                // Use emissive as the bone tint (matches rocket / tracer /
                // explosion pattern). Tame the magnitude so it doesn't
                // wash out in the lit shader.
                call.material.emissive    = color * 0.6f;
                renderer.submit(call);
            }
        }
```

- [ ] **Step 8: Cleanup on `onPeerLeft`**

Find the `onPeerLeft` callback (where `lagComp.forgetPeer(pid)`, `hostPlayers.erase(pid)`, etc. live, around line 938-953). Add:

```cpp
        // M21 — clean up active ragdoll if the leaving peer has one.
        auto rdit = activeRagdolls.find(pid);
        if (rdit != activeRagdolls.end()) {
            rdit->second->ragdoll.despawn(worldShared);
            activeRagdolls.erase(rdit);
        }
```

- [ ] **Step 9: Build**

```
cmake --build build-vk --config Debug --target net-shooter
```

Expected: clean build. If `iron::Vec3::operator*(float)` doesn't exist for `color * 0.6f`, swap to component-wise construction:
```cpp
call.material.emissive = iron::Vec3{color.x * 0.6f, color.y * 0.6f, color.z * 0.6f};
```

If `Ragdoll::boneColor` / `boneTransform` / `boneHalfExtents` / `boneBody` aren't accessible due to missing include: add `#include "physics/Ragdoll.h"` at the top of `main.cpp`.

- [ ] **Step 10: Solo smoke test**

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Solo, you're the only player so dying via self-fire (splash damage from your own rocket against a wall ~3m away) is the only way to test. Stand close to a wall, fire a rocket at it. If you take enough splash to die (rocket needs to hit the wall AT MOST 4m from you):
- Expected: your cube disappears, an 11-bone ragdoll appears at your foot position, tumbles in a direction away from the explosion, settles or falls into ragdoll-pose, then despawns after ~2s as you respawn.

If you don't take fatal damage from one rocket, fire a second one quickly. If the cooldown blocks that, accept that solo testing is limited — Step 11 covers two-process.

- [ ] **Step 11: Two-process smoke test**

```
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --connect 127.0.0.1
```

Expected:
- Client shoots host with rifle until host's HP → 0. Host's cube on the client's screen disappears, a ragdoll appears at the host's last position, **tumbles in the direction the bullet was traveling** (away from the shooter). After 2s, ragdoll despawns and host respawns at a new position.
- Host rockets client: client's cube disappears, ragdoll appears, **knocked back away from the explosion** (with upward bias). Despawns after 2s.
- Both peers see the same ragdoll appearance (give or take cosmetic divergence over the 2s).

If the impulse direction looks wrong (e.g., hitscan kills push the body TOWARD the shooter), the impulse sign needs flipping — `msg.dx/dy/dz` is the aim direction (away from shooter), so it should push the corpse away. If it pushes back, the issue is elsewhere (likely the impulse is applied to the wrong bone or with wrong magnitude).

- [ ] **Step 12: Commit**

```
git add games/07-net-shooter/main.cpp
git commit -m "M21 Task 2: death-into-ragdoll wiring (broadcast + spawn + render + despawn)"
```

---

## Task 3: Docs + PR

**Files:**
- Modify: `docs/engine/physics.md`

- [ ] **Step 1: Append M21 section to `docs/engine/physics.md`**

Read the file to find where M20 ends. Append AFTER M20 (and BEFORE the bottom "See also" / "What's next" if those exist):

```markdown

## M21 — Death-into-ragdoll in net-shooter

When a player's HP drops to 0, net-shooter now spawns a tumbling
`iron::Ragdoll` (M18) at the death position instead of instantly
hiding the corpse cube. The ragdoll lives 2 seconds — matching the
existing respawn timer — then despawns as the player respawns at a
new position.

### Per-peer `worldShared`

Where M20 had `worldShared` on the host only (containing arena +
rockets), M21 extends this so **every peer** (host + clients)
maintains its own `worldShared` populated with arena collision via
`populateArenaCollision(world, arena)`. Host's also holds rockets;
clients only ragdolls. Clients now step `worldShared` once per
render frame; the host continues to step it in its sim tick.

### `DeathMsg` wire

```cpp
struct DeathMsg {
    static constexpr std::uint8_t kTag = 11;
    std::uint32_t victimPeerId;
    float x, y, z;                          // foot position
    float impulseX, impulseY, impulseZ;     // torso impulse (pre-scaled)
};
```

Host broadcasts on every fatal hit; clients spawn a local ragdoll on
receipt. **No per-tick bone-transform sync** — ragdolls are
cosmetic-only and only live 2 seconds, so cosmetic divergence between
peers is acceptable. `kGameId` bumped from NSTS (0x4E535453) to NSTT
(0x4E535454).

### Impulse computation

At the kill site (host-side), the impulse direction comes from the
killing weapon:
- **Hitscan**: `msg.dx/dy/dz` (the normalized aim direction) × 30. The
  corpse flies away from the shooter.
- **Rocket**: `(victimPos - explosionPos).normalize()` × 50, with a
  +1.0 upward bias before normalization for satisfying knockback. The
  corpse is thrown away from the blast.

Both magnitudes are `velocity × mass` (Jolt's `applyImpulse`
convention). Torso mass ≈ 27 kg (0.36 fraction of 75 kg), so 30 N·s
≈ 1.1 m/s nudge; 50 N·s ≈ 1.85 m/s knockback. Tuned to look
realistic without rocket-jumping the corpse.

### Render integration

The existing player-cube render loops gain one extra skip — if the
peer's id is in `activeRagdolls`, the cube is not drawn. A parallel
loop renders each ragdoll's 11 bones as colored cubes via
`Ragdoll::boneTransform / boneHalfExtents / boneColor`, using
`material.emissive = color * 0.6f` (matches the rocket/tracer/
explosion tint pattern).

### Cleanup

`onPeerLeft` despawns any active ragdoll for the leaving peer.
`spawnLocalRagdoll` despawns any existing ragdoll for the same peer
before creating a new one (handles the edge case of two fatal hits
in the same tick).

### Physics overhaul track complete

M18-M21 ships the foundation for TF2/Overwatch-class movement +
shooting + death feel:
- M18 — Jolt integration + `iron::PhysicsWorld` + `iron::Ragdoll` +
  09-physics-playground
- M19 — `iron::CharacterController` + net-shooter movement port
- M20 — Net-shooter rocket projectiles → Jolt + PhysicsWorld
  extensions (raycast normal, `onContactStarted`, `velocityOf`)
- M21 — Death-into-ragdoll wiring in net-shooter

### What's next

Engine tracks with the largest remaining gap to a polished TF2/
Overwatch-class game:
- **Skeletal animation + glTF asset pipeline.** Replace capsule
  characters and cube ragdoll bones with skinned 3D meshes.
- **PBR shading upgrade.** Replace Blinn-Phong with metallic/
  roughness + bloom + tone-mapping.
- **Editor track.** A simple ImGui scene editor + scene serialization
  for content authoring.
- **Audio.** The engine has zero audio today.
- **Player-vs-projectile direct hits** + **player-vs-ragdoll
  collision** (currently absent because of M19's per-peer world
  isolation).
```

- [ ] **Step 2: Commit**

```
git add docs/engine/physics.md
git commit -m "M21 Task 3: docs/engine/physics.md M21 section + track complete"
```

- [ ] **Step 3: Push + open PR**

```
git push -u origin feat/m21-death-into-ragdoll
```

Then:

```
gh pr create --title "M21: death-into-ragdoll in net-shooter" --body "$(cat <<'EOF'
## Summary
- New `DeathMsg` wire packet (tag 11) — broadcast by host at every fatal hit; carries victim peer id, death position, and a pre-computed torso impulse vector
- Every peer (host + clients) maintains its own `iron::PhysicsWorld worldShared` (host has it from M20 with rockets; clients add a ragdoll-only one with arena collision)
- `spawnLocalRagdoll(pid, footPos, impulse)` helper used by both host loopback and client `DeathMsg` handler
- Hitscan kill impulse: `aimDir * 30` (corpse flies away from shooter); rocket kill impulse: `(victim - explosion).normalize() * 50` with +1.0 upward bias
- Ragdoll lives 2 seconds (matches existing respawn timer), then despawns
- Render loop skips the player cube while their ragdoll is active; parallel loop renders the 11 bones as colored cubes via `Ragdoll::boneTransform/boneHalfExtents/boneColor` with `material.emissive = color * 0.6`
- `kGameId` bumped NSTS → NSTT (wire-format break)
- No engine changes — uses existing M18 `iron::Ragdoll` + `iron::PhysicsWorld::applyImpulse`
- No new tests (cosmetic gameplay feature; the underlying physics is tested via M18's `test_ragdoll` + `test_physics_world`)

**Final milestone of the M18-M21 physics overhaul track.** Engine now has the foundation for TF2/Overwatch-class movement + shooting + death feel.

## Test plan
- [ ] CI green (Windows MSVC) — 37+ existing tests still pass
- [ ] Solo net-shooter: die via self-splash damage → ragdoll tumbles for 2s → respawn
- [ ] Two-process net-shooter: dying produces a tumbling ragdoll on both peers; direction reflects the killing weapon's vector (hitscan pushes corpse away from shooter; rocket knocks corpse away from explosion with upward bias)
- [ ] No regressions in M19 (movement) or M20 (rocket physics)

## Known v1 simplifications
- No bone-transform networking — ragdolls diverge cosmetically over their 2-second lifetime; invisible in normal play
- No player-vs-ragdoll collision (players walk through fresh corpses)
- Rocket-vs-ragdoll interaction visible on host only (clients have no rockets in their worldShared)
- No setGravityFactor engine helper (still flagged from M20)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- `DeathMsg` wire + `kGameId` bump → Task 1 Step 1
- Per-peer `worldShared` on clients → Task 1 Steps 3-4
- `ActiveRagdoll` state + `spawnLocalRagdoll` helper → Task 2 Step 1
- Host-side broadcast at hitscan kill site → Task 2 Step 2
- Host-side broadcast at rocket kill site → Task 2 Step 3
- Client `DeathMsg` handler → Task 2 Step 4
- 2-second despawn → Task 2 Step 5
- Cube-hide guard in render → Task 2 Step 6
- Bone rendering loop → Task 2 Step 7
- `onPeerLeft` cleanup → Task 2 Step 8
- Docs → Task 3

**Placeholder scan:** clean — every code step has actual code.

**Type consistency:**
- `iron::netshooter::DeathMsg` field names (`victimPeerId`, `x`, `y`, `z`, `impulseX/Y/Z`) consistent across Messages.h, handlers, and broadcasts
- `ActiveRagdoll` field names (`victimPeerId`, `spawnTimeSec`, `ragdoll`) consistent across declaration + spawn helper + per-frame despawn loop + render loop
- `activeRagdolls` keyed by `pid` (`std::uint32_t`) everywhere
- `iron::Ragdoll::boneBody(int idx)`, `boneTransform(int idx)`, `boneHalfExtents(int idx)`, `boneColor(int idx)`, `boneCount()` — all consistent with M18's public API

**Known v1 simplifications documented inline:** non-networked bone transforms, no player-vs-ragdoll collision, no rocket-vs-ragdoll on clients.
