# M21 — Death-into-Ragdoll in Net-Shooter (Design Spec)

**Date:** 2026-05-27
**Milestone:** M21 (final milestone of the physics overhaul track)
**Status:** Design — awaiting implementation plan

## Goal

When a net-shooter player's HP drops to 0, replace the existing "cube vanishes" effect with a tumbling 11-body `iron::Ragdoll` spawned at the death position. The ragdoll receives an impulse on the torso based on the killing weapon (hitscan ray direction or rocket explosion vector). After the existing 2-second respawn timer, the ragdoll despawns and the player respawns at a new position as before.

After M21 lands, the physics overhaul track (M18-M21) is complete. The engine has Jolt physics, character controllers, projectile physics, and ragdoll-on-death visuals — the foundation for TF2/Overwatch-class movement+shooting+death feel.

## Direction context

This is the final milestone in the M18-M21 track. M18 brought Jolt + `iron::Ragdoll`. M19 added `iron::CharacterController` and ported net-shooter movement. M20 ported net-shooter rockets to Jolt rigid bodies. M21 closes the loop by replacing the instant-disappear corpse with a satisfying ragdoll death effect.

Today's net-shooter death flow (M8.6):
- Hit detected (hitscan or rocket splash) → `iron::applyDamage(victim.hp, dmg)`.
- If `!iron::isAlive(victim.hp)`: host sets `victim.respawnAtSec = nowSec() + 2.0`, broadcasts `DamageMsg{... victimHpAfter = 0}`, kill feed pushes attacker→victim.
- Clients see `hpForHud = 0` for the victim → hide the corpse cube instantly.
- 2 seconds later, host broadcasts `RespawnMsg{victim, newSpawnPos, hp=100}`.

M21 reshapes step 2 — instead of hiding the cube, every peer (host + clients) spawns a ragdoll for the 2-second window.

## Non-Goals

- **Networked ragdoll bone-transform sync.** Each peer simulates locally; cosmetic divergence after the first physics step is acceptable for a 2-second visual. No per-tick bone broadcasts.
- **Custom death animations.** Pure rigid-body simulation. Skeletal animation (and rendering a skinned mesh on top of the ragdoll bones) is a future track.
- **Ragdoll persists past respawn.** The ragdoll despawns exactly when the respawn happens (2s timer). No corpse clutter.
- **Player-vs-ragdoll collision.** Ragdolls live in the shared physics world; characters live in per-peer worlds (M19 isolation). Players walk through fresh corpses. Future track if needed.
- **Force pickups, gibbing, body parts.** Just one humanoid ragdoll per death.
- **Anti-cheat changes.** Hitscan + rocket damage validation logic is M20's responsibility; M21 trusts what's already there.

## Architecture

### 1. Per-peer `worldShared` (every peer now has one)

M20 introduced a host-only `iron::PhysicsWorld worldShared` containing arena boxes + projectile bodies. M21 extends this: **every peer** (host AND clients) creates a `worldShared` populated with arena collision via the existing `populateArenaCollision(world, arena)` helper.

Host's `worldShared` contains: arena geometry + active rocket bodies (M20) + active ragdoll bodies (M21).
Client's `worldShared` contains: arena geometry + active ragdoll bodies (no rockets — clients use linear extrapolation for ghost rockets).

Clients now also step `worldShared(dt)` per frame in the render loop (host already does this in the sim tick from M20).

### 2. New wire message: `DeathMsg`

```cpp
// Host -> Clients: broadcast when a player dies (HP -> 0).
struct DeathMsg {
    static constexpr std::uint8_t kTag = 11;   // next free after ScoreUpdate (10)
    std::uint32_t victimPeerId;
    float x, y, z;                              // death foot position
    float impulseX, impulseY, impulseZ;         // impulse vector for torso (already
                                                 // scaled — no per-client computation)
};
```

`kGameId` bumps from `0x4E535453u` (NSTS, M19) to `0x4E535454u` (NSTT) to lock out old clients.

### 3. Per-peer ragdoll state

```cpp
struct ActiveRagdoll {
    std::uint32_t victimPeerId;
    iron::Ragdoll ragdoll;
    double        spawnTimeSec;
};
std::unordered_map<std::uint32_t, ActiveRagdoll> activeRagdolls;
```

One map per peer. Keyed by `victimPeerId` so a quick lookup tells the renderer "is this peer currently a ragdoll" without iterating.

### 4. Host: compute impulse at kill site

Two kill sites in `main.cpp` need the impulse computation:

**Hitscan kill** (`FireHitscanMsg` handler, around line 1127-1146):

```cpp
if (!iron::isAlive(vit->second.hp)) {
    // ... existing kill bookkeeping ...

    // M21 — compute death impulse + broadcast DeathMsg.
    const iron::Vec3 hitDir = iron::Vec3{msg.dx, msg.dy, msg.dz};  // already normalized
    constexpr float kHitscanImpulseMag = 30.0f;  // m/s ⋅ kg (10 m/s × 3 kg torso mass)
    const iron::Vec3 impulse{
        hitDir.x * kHitscanImpulseMag,
        hitDir.y * kHitscanImpulseMag,
        hitDir.z * kHitscanImpulseMag,
    };
    const iron::Vec3 deathPos{
        vit->second.lastFootPos.x,
        vit->second.lastFootPos.y,
        vit->second.lastFootPos.z,
    };
    peers.broadcastToAll<iron::netshooter::DeathMsg>(
        iron::netshooter::DeathMsg{
            dm.victimPeerId,
            deathPos.x, deathPos.y, deathPos.z,
            impulse.x, impulse.y, impulse.z,
        },
        iron::SendReliability::Reliable);
    spawnLocalRagdoll(dm.victimPeerId, deathPos, impulse);  // host's own ragdoll
}
```

**Rocket kill** (rocket despawn handler, around line 1567-1601):

```cpp
if (!iron::isAlive(vit->second.hp)) {
    // ... existing kill bookkeeping ...

    // M21 — compute death impulse from explosion → victim direction.
    const iron::Vec3 vpos = vit->second.lastFootPos;
    iron::Vec3 fromExplosion{
        vpos.x - d.point.x,
        vpos.y - d.point.y + 1.0f,  // bias upward so torsos tend to fly up
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
        fromExplosion = iron::Vec3{0.0f, 1.0f, 0.0f};  // straight up fallback
    }
    constexpr float kRocketImpulseMag = 50.0f;
    const iron::Vec3 impulse{
        fromExplosion.x * kRocketImpulseMag,
        fromExplosion.y * kRocketImpulseMag,
        fromExplosion.z * kRocketImpulseMag,
    };
    peers.broadcastToAll<iron::netshooter::DeathMsg>(
        iron::netshooter::DeathMsg{
            pid,
            vpos.x, vpos.y, vpos.z,
            impulse.x, impulse.y, impulse.z,
        },
        iron::SendReliability::Reliable);
    spawnLocalRagdoll(pid, vpos, impulse);  // host's own ragdoll
}
```

> **`lastFootPos`** — a new field on `HostPlayer` / `RemotePlayer` (or pulled from `authStates[pid]` for HostPlayer) that tracks the last known foot position. For the host, `authStates[pid]` already has `x/y/z` — read from there. No new field needed if we just `Vec3{authStates[pid].x, authStates[pid].y, authStates[pid].z}` at the kill site.

> **Constants `kHitscanImpulseMag = 30.0` and `kRocketImpulseMag = 50.0`:** picked to look right. `applyImpulse(body, v)` in Jolt expects a `velocity * mass` vector — torso mass = 0.36 × 75kg = 27kg, so 30 N·s gives ~1.1 m/s sideways from hitscan (subtle nudge). Rocket gives ~1.85 m/s (knockback). Both are intentionally small so the ragdoll tumbles realistically rather than rockets to the moon.

### 5. `spawnLocalRagdoll(victimPeerId, footPos, impulse)` (every peer)

```cpp
auto spawnLocalRagdoll = [&](std::uint32_t pid, iron::Vec3 footPos,
                              iron::Vec3 impulse) {
    // Despawn any existing ragdoll for this peer (rare edge case: player
    // takes two fatal hits in the same tick).
    auto existing = activeRagdolls.find(pid);
    if (existing != activeRagdolls.end()) {
        existing->second.ragdoll.despawn(worldShared);
        activeRagdolls.erase(existing);
    }

    ActiveRagdoll ar;
    ar.victimPeerId = pid;
    ar.spawnTimeSec = nowSec();
    ar.ragdoll.spawn(worldShared, iron::RagdollSpec{}, footPos);
    worldShared.applyImpulse(ar.ragdoll.boneBody(iron::Ragdoll::kTorso), impulse);
    activeRagdolls.emplace(pid, std::move(ar));
};
```

> The `std::move(ar)` requires `iron::Ragdoll` to be movable — verify in implementation. If not, store as `std::unique_ptr<ActiveRagdoll>` instead (same pattern as `HostPlayerSim` from M19).

### 6. Client: `DeathMsg` handler

```cpp
registry.registerHandler<iron::netshooter::DeathMsg>(
    [&](iron::ConnectionId, const iron::netshooter::DeathMsg& msg) {
        if (peers.isHost()) return;  // host already spawned locally
        spawnLocalRagdoll(
            msg.victimPeerId,
            iron::Vec3{msg.x, msg.y, msg.z},
            iron::Vec3{msg.impulseX, msg.impulseY, msg.impulseZ});

        // Also mark the remote player as dead for cube-hiding logic.
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

### 7. Per-frame: step + despawn

In the render loop (every peer):

```cpp
// M21 — step worldShared on the client side. Host already steps it in
// the sim tick from M20.
if (!peers.isHost()) {
    worldShared.step(frameDt);
}

// Despawn ragdolls older than 2s (matches respawn timer).
const double now = nowSec();
for (auto it = activeRagdolls.begin(); it != activeRagdolls.end(); ) {
    if (now - it->second.spawnTimeSec > 2.0) {
        it->second.ragdoll.despawn(worldShared);
        it = activeRagdolls.erase(it);
    } else {
        ++it;
    }
}
```

### 8. Render integration

In the player-rendering loop (where each peer's cube is drawn), skip rendering the cube if their peerId is in `activeRagdolls`:

```cpp
for (const auto& [pid, remote] : remotes) {
    if (activeRagdolls.find(pid) != activeRagdolls.end()) continue;  // ragdoll instead
    if (remote.hpForHud <= 0) continue;  // M8.6 corpse hide (still applies for late-arriving DamageMsg)
    // ... existing cube rendering ...
}
```

Then add a separate loop that renders ragdolls:

```cpp
for (const auto& [pid, ar] : activeRagdolls) {
    for (int i = 0; i < ar.ragdoll.boneCount(); ++i) {
        // Same submitBox(model, halfExtents, color) pattern as 09-physics-playground.
        const iron::Mat4 model = ar.ragdoll.boneTransform(i);
        const iron::Vec3 he    = ar.ragdoll.boneHalfExtents(i);
        const iron::Vec3 color = ar.ragdoll.boneColor(i);
        // ... use existing renderer.submit() with whatever material/shader the
        //     net-shooter player cubes use, tinted by `color`.
    }
}
```

The exact tint-per-bone approach depends on whether net-shooter's existing player shader supports a per-DrawCall tint. If it doesn't, the simplest path is to feed `color` into `material.emissive` (the same hack the playground uses for distinct ragdoll bones, with the M18-fixup palette approach using 1×1 textures). Decision deferred to implementation — pick whichever requires the smallest change to existing rendering code.

### 9. Cleanup on `onPeerLeft`

```cpp
auto rdit = activeRagdolls.find(pid);
if (rdit != activeRagdolls.end()) {
    rdit->second.ragdoll.despawn(worldShared);
    activeRagdolls.erase(rdit);
}
```

Add alongside the existing peer-cleanup (which already handles `hostSims`, `hostRockets`, `lagComp.forgetPeer`).

## Tasks

Three subagent-friendly chunks:

1. **`DeathMsg` + per-peer `worldShared` on clients** — add the wire message, bump `kGameId`, ensure clients init a `worldShared` with arena collision at startup (host already has it from M20). Step `worldShared` in client render loop. Standalone — no death-handling yet.

2. **`spawnLocalRagdoll` + DeathMsg handler + host-side broadcast at kill sites + per-frame despawn + render** — all the wiring. Host computes impulse + broadcasts + spawns locally; clients receive + spawn locally; both render ragdoll bones + skip the player cube while ragdoll active; both clean up at 2s; both clean up on peer-leave.

3. **Docs** — append M21 section to `docs/engine/physics.md`. Cap the physics overhaul track with a "what's next after physics" summary.

## Tests

No new engine tests needed — `iron::Ragdoll` + `iron::PhysicsWorld::applyImpulse` are exercised by existing tests from M18.

Game-side: manual visual smoke tests only.

**Solo smoke test (`--listen`):**
- Aim at a wall, fire a rocket. The rocket hits the wall + explodes — you take splash damage. If splash kills you (test by stripping HP first via `--debug-low-hp` flag if exists, or by standing very close), a ragdoll appears at your foot position, tumbles for 2s, then disappears as you respawn.
- This will only work for the local player — the host needs at least one other peer to die for the more-interesting case.

**Two-process smoke test:**
- Host + client on `127.0.0.1`.
- Client shoots host with rifle until host dies. Host's cube disappears, ragdoll appears, tumbles based on shot direction (host should be pushed AWAY from the client — verify impulse direction is correct), respawns after 2s.
- Host rockets client. Client's cube disappears, ragdoll appears, knockback away from explosion, respawns.

Subjective polish targets:
- Impulse magnitudes feel right (not too floppy, not rocket-jumping)
- 2s window feels neither too short (corpse barely lands) nor too long (waiting feels dead)
- Bone colors distinguishable enough that the human shape reads at a glance

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| `iron::Ragdoll` not movable → `activeRagdolls.emplace(pid, std::move(ar))` fails | Either make `Ragdoll` movable in M21 Task 2 (`= default` move ops on the class — its only members are `std::array` of POD), or store as `std::unique_ptr<ActiveRagdoll>` |
| Client `worldShared.step(frameDt)` uses variable dt → non-determinism | Acceptable — ragdolls are cosmetic; non-deterministic look is fine. Host's `worldShared.step` already uses fixed sim dt from M20; client's frame dt is variable. Documented. |
| Ragdoll spawned at death-pos clips into floor for 1 frame | Hips spawn at `footPos.y + 0.42` (from `RagdollSpec`'s anatomy table); floor is at `y = 0`. No clipping. |
| Two fatal hits in the same tick → existing ragdoll for peer → leak | `spawnLocalRagdoll` despawns existing entry first (idempotent reset). |
| Hitscan impulse direction (`{msg.dx, msg.dy, msg.dz}`) might not be unit-length on the wire | Spec says it's normalized at client side before sending. Verify in implementation; if not, normalize at the kill site before scaling by magnitude. |
| Rocket explosion at exact victim position → impulse direction is zero | Fallback to `Vec3{0, 1, 0}` (straight up) in the code above. |
| `kGameId` bump locks out old clients | Documented; same pattern as M19. |
| `worldShared.step` cost on clients is new work | One arena (~14 static boxes) + maybe 1-2 ragdolls (22 bodies + 20 joints max) = negligible CPU. |
| Render path for ragdoll bones needs a tint mechanism | Use the existing player-cube shader with `material.emissive = color * <small factor>` (matches the playground demo's tint workaround). If the result looks washed out, swap to a real diffuse-texture palette like the playground's fixup. |

## Verification

- **CI green** — no new tests, but existing 37+ ctest tests must still pass after the wire-format break.
- **Solo net-shooter:** rockets explode + splash damage works; if a splash kills, a ragdoll appears + tumbles + respawn.
- **Two-process net-shooter:** dying produces a tumbling ragdoll on both peers; direction reflects the killing weapon's vector.
- **No regressions** in M19 (movement) or M20 (rocket physics).

## Follow-ups (NOT in M21)

- Skeletal animation / 3D character meshes (replaces capsule + cube-bone visuals).
- Hitscan-on-Jolt swap (currently engine `Ray` math — fine for arena AABBs; would matter once ragdoll bones are valid hitscan targets).
- Networked ragdoll sync (if the cosmetic divergence becomes objectionable in practice).
- Ragdoll-pushes-player gameplay (currently no player↔ragdoll physics interaction because they live in different worlds).
- `setGravityFactor(BodyId, float)` engine helper (still flagged from M20).

---

**M18-M21 physics overhaul track summary (after M21 lands):**

- M18 — Jolt integration + `iron::PhysicsWorld` + `iron::Ragdoll` + 09-physics-playground
- M19 — `iron::CharacterController` + net-shooter movement port
- M20 — Net-shooter rocket projectiles → Jolt + PhysicsWorld extensions
- M21 — Death-into-ragdoll wiring in net-shooter

Engine has a complete TF2/Overwatch-class movement + shooting + death-feel foundation. Next likely tracks: skeletal animation + glTF asset pipeline, PBR shading upgrade, or the editor track.
