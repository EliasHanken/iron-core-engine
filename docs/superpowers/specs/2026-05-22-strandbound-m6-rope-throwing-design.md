# Strandbound M6 — "Rope Throwing" — Design

**Status:** Approved 2026-05-22.

## Goal

Make bridging the gap a skill. Today the player places an anchor with an
instant, infinite-range raycast and ties ropes between anchors — so crossing
the gap is trivial. M6 replaces placement and tying with a single skill verb:
**throw the rope**. The player charges a throw, the rope's far end flies in an
arc, and it sticks where it lands. A finite **rope pool** caps how many ropes
are deployed at once.

After M6 the Strandbound demo is genuinely playable: you must *land* the bridge,
not just point at where you want it.

## Context

Milestones M1–M5 plus the HUD overlay milestone are complete and merged. The
engine has a window + OpenGL 3.3 renderer (lit path, debug lines, dynamic
meshes, a screen-space HUD), a hand-written math library, raycasting
(`intersectRaySphere`, `intersectRayAabb`), Verlet rope physics, a
`FirstPersonController`, and the M5 tightrope traversal (`RopeWalker`).

The game `games/02-strandbound` currently lets the player place free-floating
anchors (right-click, an instant raycast against the world boxes) and tie ropes
between two anchors (left-click). `RopeTool` owns the anchors, the ropes, and
the place / tie / cut logic and draws them. `RopeWalker` then lets the player
mount a tied rope and tightrope-walk it; reaching the far island wins.

M6 changes only **how a rope comes to exist**. Everything downstream — the
rope as a `Rope`, mounting it, the tightrope traversal, footing, the win — is
unchanged.

Development stays game-driven: M6 is **entirely game-side**. It needs no engine
change — projectile gravity is a few lines, collision reuses
`intersectRayAabb`, the rope is the existing `Rope`, and the charge bar and
rope count reuse the HUD.

## Scope

### In scope

- A new game-side **`RopeThrower`** — a headless, unit-tested charge → throw →
  projectile → land/fail state machine.
- `RopeTool` reworked: it loses placement, tying, and free anchors; it keeps
  the rope collection, drawing, and cut, and gains a **rope pool** (a finite
  count of available ropes).
- `main.cpp` wiring: hold left-click to charge, release to throw; the HUD
  charge bar and the rope-count readout.
- Drawing the in-flight projectile and a small marker at each rope endpoint.

### Out of scope (deliberate)

- **No engine changes.** M6 is game-side only.
- **Rope tightening / a "pull" verb** — a good future mechanic, not M6.
- **A predicted-landing arc or aim indicator** — deliberately omitted; the
  skill is reading the throw by feel and watching the projectile fly.
- **A trailing/extending rope during flight** — only the projectile (the rope's
  far end) flies; the rope appears as a `Rope` once the far end sticks.
- **Environment / atmosphere work** — fog, sky, reflections, ground materials,
  the texture-stretching fix, and a default asset pack are a separate
  milestone after M6.
- **A restart flow** — unchanged from M5; re-running the executable restarts.

## Design

### Architecture

`RopeThrower` (new) owns the throw. `RopeTool` (reworked) owns the rope
*collection*. They mirror the M5 split between `RopeWalker` (a focused,
headless, testable game class) and `RopeTool` (the GL-side collection):

- **`RopeThrower`** — charge level, the in-flight projectile (a point under
  gravity), and collision against the world boxes. Pure logic, no GL, no
  rendering — headless and unit-tested. Each step it reports an event; on a
  landing it yields the near and far endpoints of the rope to create.
- **`RopeTool`** — owns the `Rope` list and the **rope pool** count, draws the
  rope tube meshes and the endpoint markers, handles cut, and exposes
  `ropes()` for `RopeWalker`. Placement, tying, free anchors, and the aim
  marker are removed.

### 1. `RopeThrower` — the throw state machine

A small state machine: **Idle → Charging → InFlight → (Idle)**.

`RopeThrower::update` is called every fixed step with: whether the throw button
is held, whether a rope is available (pool non-empty), the player's eye
position and look direction, the player's feet position, the world box
colliders, and `dt`. It returns an **event** for the step: `None`, `Landed`, or
`Missed`.

- **Idle → Charging.** When the button is held, a rope is available, and the
  button has been released since the last throw, charging begins. `charge`
  (0…1) ramps up over a fixed charge time, then holds at 1 (holding longer is
  not punished).
- **Charging → InFlight (release).** Releasing the button launches the
  projectile: its position starts at the player's **eye**, its velocity is the
  **look direction** times a speed interpolated from `charge`
  (`minThrowSpeed` … `maxThrowSpeed`). The player's **feet** position at this
  moment is stored as the rope's near end.
- **InFlight.** Each step the projectile integrates under gravity (a point +
  velocity). The travel segment for the step is tested against the world boxes
  with `intersectRayAabb` (as a segment test — a hit counts only within the
  step's length).
  - **Hit → `Landed`.** The projectile sticks at the impact point. The thrower
    exposes the near end (stored feet) and far end (impact point); the game
    creates the rope. The thrower returns to Idle.
  - **No hit, projectile falls below the kill plane → `Missed`.** The throw
    failed; no rope, nothing spent. The thrower returns to Idle.
- Only **one throw is in flight at a time**; charging cannot begin again until
  the button has been released since the last launch.

`RopeThrower` exposes `state()`, `charge()` (for the HUD bar),
`projectilePosition()` (for drawing the in-flight marker), and — valid on a
`Landed` event — `ropeNearEnd()` / `ropeFarEnd()`.

The charge ramp, the charge→speed mapping, the projectile gravity step, and the
segment-vs-AABB collision are pure functions or pure `RopeThrower` steps — all
unit-tested headless.

### 2. `RopeTool` — the rope collection and the pool

`RopeTool` is reworked:

**Removed** — `anchors_` and all free-anchor logic; `pickAnchor`,
`pickSurface`, `refreshAimTarget`; the place and tie verbs; `tyingFromAnchor_`;
the aim marker and `clearAimTarget`; `anchorCount()`.

**Kept** — `ropes_`, the per-step rope Verlet update, `draw` (the rope tube
meshes), `pickRope` (used by cut), cut, and `ropes()`.

**Added** — a **rope pool**: `ropesAvailable_`, an integer initialised to a
small starting count.
- `bool addRope(Vec3 nearEnd, Vec3 farEnd)` — if the pool is non-empty,
  constructs a `Rope` between the two points, appends it, decrements the pool,
  and returns true; otherwise returns false.
- Cutting a rope removes it and **increments** the pool (a cut rope is
  recovered).
- `int ropesAvailable() const` — for the HUD readout.

`draw` additionally draws a small debug-line marker at each rope's two
endpoints, so the player can see where a rope can be mounted.

The rope pool starts at a small count (a handful — tuned during playtest). A
missed throw never touches the pool; only a landed rope spends one, and cutting
refunds one. There is no soft-lock: a missed throw is free.

### 3. `main.cpp` wiring

The game owns a `RopeThrower`. The level's box colliders (already built) are
passed to it each frame.

In the `Walking` player state (M5's state machine is unchanged):
- Left-click **held** → `RopeThrower` charges; **released** → it throws.
- `RopeThrower::update` runs each step. On a `Landed` event the game calls
  `ropeTool.addRope(thrower.ropeNearEnd(), thrower.ropeFarEnd())`.
- `C` still cuts (now also refunds a rope).
- Right-click is no longer used.

`RopeWalker` mounting, traversal, footing, respawn, and the win are entirely
unchanged — a thrown-and-landed rope is an ordinary `Rope`.

### 4. HUD

Reusing the HUD subsystem (no engine work):
- A **charge bar** — a track panel plus a fill panel whose width tracks
  `charge` (0→1), shown only while the thrower is `Charging`, hidden otherwise.
  Same pattern as the M5 lean meter.
- The **rope-count readout** — the existing top-left text changes from
  `Anchors: N   Ropes: M` to `Ropes: N`, where N is `ropesAvailable()`.
- The **crosshair** stays. There is no predicted-landing indicator.

The in-flight projectile is drawn as a small debug-line marker at
`projectilePosition()` while the thrower is `InFlight`.

### File layout

```
games/02-strandbound/RopeThrower.h, RopeThrower.cpp   charge + projectile + collision  (new)
games/02-strandbound/RopeTool.h, RopeTool.cpp         drop place/tie/anchors; add pool (modified)
games/02-strandbound/main.cpp                         wire RopeThrower; charge input; HUD (modified)
games/02-strandbound/CMakeLists.txt                   register RopeThrower.cpp         (modified)
tests/test_rope_thrower.cpp                           charge / arc / collision / outcomes (new)
tests/CMakeLists.txt                                  register the test               (modified)
docs/engine/strandbound-m6.md                         concept note                    (new)
```

## Testing

`RopeThrower` is headless — it is unit-tested in the CTest harness, compiling
`RopeThrower.cpp` directly into the test (the pattern used for
`test_rope_walker`):

- **Charge ramp** — holding the button advances `charge` toward 1 over the
  charge time and then holds at 1; releasing before launch maps `charge` to a
  throw speed between `minThrowSpeed` and `maxThrowSpeed`.
- **Projectile arc** — a launched projectile's height rises then falls under
  gravity; horizontal travel is monotonic.
- **Collision** — a throw aimed into a box collider produces a `Landed` event
  with an impact point on that box; the reported near end is the feet position
  captured at release.
- **Miss** — a throw that hits nothing falls below the kill plane and produces
  a `Missed` event; no endpoints are produced.
- **State / pool gating** — charging does not begin when no rope is available;
  a second throw cannot begin until the button is released.

`RopeTool`'s rework is GL-side (it owns renderer resources) and, like the
existing `RopeTool`, is verified by running the game. Its pool is trivial count
logic. The throw feel, the charge bar, and the on-screen result are verified by
running the game, as in every milestone.

## Acceptance criteria

Launch `games/02-strandbound`. The player can no longer place anchors by
pointing — instead, holding left-click charges a throw (a HUD power bar fills),
and releasing throws the rope's far end in an arc. A throw that lands on a
solid surface creates a rope from the player's feet to the impact point; a
throw that falls into the gap fails with nothing lost. The HUD shows the
remaining rope count; it drops when a rope lands and rises when one is cut.
Once a rope bridges the gap, mounting and tightrope-walking it to the far
island still works and still wins. `Escape` quits.

## Conventions

Unchanged from M1–M5: namespace `iron` for engine code (game classes are
global, like `RopeTool` and `RopeWalker`); engine headers included relative to
`engine/`; `Mat4` column-major; C++23; CMake; commit after every task with the
`Co-Authored-By` trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`. Work proceeds on a
feature branch; `main` is protected (PR + green CI required to merge).
