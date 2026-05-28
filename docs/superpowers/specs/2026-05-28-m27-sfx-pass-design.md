# M27 — SFX Pass + Fox-Sized Player (Design)

**Date:** 2026-05-28
**Status:** Approved
**Predecessors:** M25 (skinned characters), M26 (audio foundation)
**Successors:** M28 — Spatial audio (reverb / occlusion / looping API / music + ambient zones)

## Goal

Make net-shooter sound like a real game. Add 8 SFX events on top of M26's audio foundation (gunshot, rocket-launch, jump, hit, death scream, 4 footstep variants picked randomly). Also right-size the player to match the fox model (smaller hitbox, lower camera) so movement and combat feel consistent with what the eye sees. All changes are game-side — no engine API extensions.

## Non-Goals

- **Music and ambient loops** — need a looping audio API. Deferred to M28 where the looping API lands alongside reverb so the architecture is coherent.
- **Reverb / EFX** — OpenAL EFX extension work; M28.
- **Occlusion** — raycast from listener to source through PhysicsWorld; M28.
- **Hit-zone differentiation (headshot etc)** — needs M21's ragdoll bone hit detection wired into damage; future milestone.
- **Source URL fields in `vcpkg.json` for game assets** — assets stay in repo (LFS).

## Asset Inventory

All CC0, mono 16-bit PCM WAV, ≤ 1 second each, vendored under `games/07-net-shooter/assets/sfx/`. LFS-tracked automatically via existing `.gitattributes`.

| File | Approx length | Trigger | Per-play gain |
|---|---|---|---|
| `gunshot-rifle.wav` | ~0.3s | hitscan fire | 1.0 |
| `rocket-launch.wav` | ~0.5s | rocket fire | 0.9 |
| `footstep-01.wav` | ~0.2s | foot strike (variant 1 of 4) | 0.4 |
| `footstep-02.wav` | ~0.2s | foot strike (variant 2 of 4) | 0.4 |
| `footstep-03.wav` | ~0.2s | foot strike (variant 3 of 4) | 0.4 |
| `footstep-04.wav` | ~0.2s | foot strike (variant 4 of 4) | 0.4 |
| `hit.wav` | ~0.2s | taking damage | 0.8 |
| `jump.wav` | ~0.3s | local jump + remote grounded:true→false | 0.5 |
| `death.wav` | ~0.5s | when victim's HP reaches 0 | 1.0 |

Total: 9 vendored WAVs (5 new categories + 3 footstep variants + 1 death scream). All CC0 from freesound.org / opengameart.org.

## Trigger Logic

### Weapon fire (gunshot + rocket-launch)

- **Host:** when handling `FireHitscanMsg` / `FireRocketMsg`, play at the firing player's position. (Same site that processes the fire intent.)
- **Local client:** play at own muzzle position immediately on input (don't wait for the host roundtrip — feels laggy otherwise).
- **Other clients:** receive the host's broadcast of damage / rocket-spawn and play at the firing position. The existing `SpawnProjectileMsg` carries the spawn position; hitscan currently doesn't broadcast a "fired" message — adding one is overkill, so for hitscan: remote peers hear the gunshot via the existing `DamageMsg` broadcast (attacker position is not in the message, but the impact position is — gunshot plays at impact site for remote peers, slightly imperfect but acceptable for v1).

### Footsteps (multi-variant, state-driven)

Per peer, track `lastFootstepAtSec[pid]` and `footstepVariantState[pid]` (a tiny PRNG state, e.g., a `std::uint32_t` xor-shift seed initialized from pid). Each frame:

- Derive movement state from velocity + grounded (already done for the fox animator).
- If state is `"walk"` and `now - lastFootstepAtSec[pid] > 0.5f` → play a random variant, update `lastFootstepAtSec`.
- If state is `"run"` and `now - lastFootstepAtSec[pid] > 0.3f` → play a random variant.
- If state is `"idle"` or `!grounded` → leave `lastFootstepAtSec` alone (next step plays immediately on movement resume).

Random variant pick: `variantIndex = (xorshift(footstepVariantState[pid]) % 4)`. The PRNG is per-peer so different peers' footstep cadences don't lockstep into a single rhythm.

Position = peer foot position.

### Hit

In the `DamageMsg` handler:

- Play `hit.wav` at the victim's position.
- If `msg.victimHpAfter == 0` → also play `death.wav` at the victim's position.

### Jump

- **Firing client:** at the input-tick where `in.jump = 1`, play `jump.wav` at own foot position. Local immediacy.
- **Remote peers:** in the `AuthorityPositionMsg` handler, detect a `grounded: true → false` transition on `remotes[pid].lastGrounded`. When detected, play `jump.wav` at the broadcast position. Stash `bool lastGrounded` per `RemotePlayer` to track transitions.

Don't fire jump SFX on `grounded: false → true` (that's a landing — separate SFX, deferred).

## Fox-Sized Player

Three constants change. All game-side; no protocol break (the changed half-extents only affect calculations, not wire fields).

| Constant | Old | New | Reason |
|---|---|---|---|
| `kPlayerHalfExtents` (Messages.h) | `{0.4, 1.0, 0.4}` | `{0.25, 0.35, 0.5}` | 0.5m wide, 0.7m tall, 1.0m long — matches fox proportions (foxes are longer than tall) |
| `kEyeHeight` (main.cpp:790) | `1.6f` | `0.5f` | Fox eye height. Camera lowers ~1m. |
| `CharacterControllerConfig` defaults | `r=0.30, hh=0.90` | `r=0.25, hh=0.35` | Capsule shrinks to fit the new half-extents. Jump force unchanged. |

Side effects to verify after the change:
- Rocket splash damage radius still meaningful (it's hardcoded; check feel)
- Hitscan rays from `eyePos()` still hit walls + other foxes at expected positions
- Character can still walk through arena doorways (the doorways were sized for human players — getting narrower-than-needed isn't a regression)
- Z-extent is 1.0m so the fox's body extends 0.5m forward of its foot position — this matches the fox model rendered at scale 0.01

`kPlayerHalfExtents` is sent over the wire IMPLICITLY (peers use the constant value when reconstructing remote player AABBs for hit detection). Since both sides recompile with the same new constant, this is fine.

## Files Changed

**Create:**
- `games/07-net-shooter/assets/sfx/gunshot-rifle.wav`
- `games/07-net-shooter/assets/sfx/rocket-launch.wav`
- `games/07-net-shooter/assets/sfx/footstep-01.wav`
- `games/07-net-shooter/assets/sfx/footstep-02.wav`
- `games/07-net-shooter/assets/sfx/footstep-03.wav`
- `games/07-net-shooter/assets/sfx/footstep-04.wav`
- `games/07-net-shooter/assets/sfx/hit.wav`
- `games/07-net-shooter/assets/sfx/jump.wav`
- `games/07-net-shooter/assets/sfx/death.wav`

**Modify:**
- `games/07-net-shooter/Messages.h` — `kPlayerHalfExtents` constants.
- `games/07-net-shooter/main.cpp` — load 9 SFX; trigger sites for gunshot/rocket/jump/hit/death/footsteps; per-peer footstep timer + variant PRNG; `lastGrounded` on RemotePlayer; lowered `kEyeHeight`; new `CharacterControllerConfig` defaults.
- `engine/physics/CharacterController.h` (or wherever the config defaults live) — adjust the default `radius`/`halfHeight` for the new fox-sized player. If shared with other games, leave defaults alone and pass explicit values from net-shooter.
- `docs/engine/asset-pipeline.md` — append M27 section (audio asset pipeline grew, plus the fox-sized player note for game authors).

**No new tests** — M26 covers AudioEngine. M27 is purely game wiring + content.

## Network Considerations

**Zero new packets.** All SFX triggers piggy-back on existing messages:

| Trigger | Existing message used |
|---|---|
| Gunshot (remote-attacker) | `DamageMsg` |
| Rocket launch (remote-attacker) | `SpawnProjectileMsg` |
| Footsteps | Position-derived from `AuthorityPositionMsg` |
| Hit / death | `DamageMsg` |
| Remote jump | `AuthorityPositionMsg.grounded` transition |
| Local-anything | Triggered client-side immediately |

No game-id bump.

## Volume / Mixing Strategy

Per-call gain at the call site, no buses or master mixer for v1 (matches M26's single-gain approach). Values tuned by the implementer during visual verification, starting from the table above.

If footsteps prove too loud or too quiet in playtesting, change the constant — don't add complexity.

## Test Plan

**No unit tests added** — AudioEngine is covered in M26; new SFX wiring is mechanical and verified by play.

**Visual / playtest verification (after Task N):**
- Host + client, both connect.
- Walk around: each peer hears footsteps from the other at appropriate cadence. Pause: footsteps stop immediately. Resume: footstep plays immediately (not after one full cadence delay).
- Pre-recorded variants don't sound robotic at extended walking.
- Fire rifle: both peers hear gunshot (caller hears local muzzle; target hears at impact).
- Fire rocket: both peers hear launch (caller local muzzle; target hears at spawn). Explosion still works via M26.
- Take damage: hit sound plays for both. On killing blow, death scream plays additionally.
- Jump: caller hears jump immediately; remote peer hears jump on the `grounded:true→false` edge.
- Fox-sized: camera is noticeably lower; hitbox feels right; player can still navigate the arena.

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Footstep cadence sounds too patterned with 4 variants | Random pick + xor-shift; cadence varies slightly per state (walk vs run); deferring more variants is cheap (drop more files into `sfx/`). |
| Remote gunshot at impact position (not muzzle) sounds wrong | Acceptable v1 tradeoff; broadcasting a "fired" message would cost bandwidth for marginal fidelity. Revisit when other audio improvements land in M28. |
| Lowered camera makes movement feel sluggish (eye height affects perceived speed) | Tune `kEyeHeight` empirically; 0.5m is a starting estimate. |
| Smaller hitbox makes the player harder to hit | Intended — the visible mesh defines what's hit. |
| LFS storage of 9 small WAVs adds repo size | All small (< 100 KB each); total < 1 MB; LFS handles it fine. |
| OpenAL source pool (32) busy with footsteps during a firefight | Voice-stealing handles it. If exhaustion becomes routine, M28 can grow the pool. |

## Verification Command

```powershell
cmake --build build-vk --target net-shooter --config Debug
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
# second terminal:
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
```

Walk, shoot, jump, get killed — verify all 9 SFX events fire at the right times and positions, and the camera/hitbox feel like a fox.
