# Strandbound M6 — Rope Throwing

M6 makes bridging the gap a skill. Earlier milestones placed an anchor with an
instant, infinite-range raycast — so crossing was trivial. M6 replaces
placement and tying with a single verb: throw the rope.

## RopeThrower

`RopeThrower` (game-side, headless, unit-tested) is a small state machine —
`Idle → Charging → InFlight`:

- **Charging** — holding the throw button ramps a `charge` value 0→1 over a
  fixed time; a HUD bar shows it.
- **Launch** — releasing flings the rope's far end from the player's eye, in
  the look direction, at a speed mapped from `charge`.
- **InFlight** — the far end is a projectile under gravity. Each step its
  travel segment is tested against the world boxes (`intersectRayAabb`).
- **Outcome** — a hit reports `Landed` with the rope's endpoints (near = the
  player's feet at release, far = the impact point); falling into the void
  reports `Missed`.

A held button cannot chain throws — it must be released between throws.
Mounting a rope cancels any pending throw.

## The rope pool

`RopeTool` holds a finite pool of ropes. A landed throw spends one
(`addRope`); cutting a deployed rope (`C`) refunds one. A missed throw costs
nothing. The HUD shows the remaining count. There is no soft-lock.

`RopeTool` otherwise just owns the rope collection, draws the tube meshes and
endpoint markers, and steps the rope physics — placement, tying, and free
anchors are gone.

## Unchanged

Once a rope exists it is an ordinary `Rope`: mounting it and tightrope-walking
it (M5's `RopeWalker`), footing, respawn, and the win are all unchanged. M6 is
entirely game-side — no engine change.
