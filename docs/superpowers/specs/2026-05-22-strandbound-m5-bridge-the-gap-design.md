# Strandbound — M5 "Bridge the Gap" — Design

**Status:** Approved 2026-05-22.

## Goal

The final milestone of the playable Strandbound mechanic demo. M5 turns the
sandbox built by M1–M4 (a lit island, a first-person character, Verlet ropes,
place/tie/cut) into an actual *game with a goal*: the player builds a rope
bridge across a gap, walks it as a **tightrope-balance challenge**, and wins by
reaching a second island.

Roadmap line (from the M2 spec): *"Cross the gap on a rope; the gap becomes
real."*

## Context

Milestones M1–M4, the "Solid Ropes & Anchors" visual milestone, and the HUD
overlay milestone are all complete and merged to `main`. The engine has: a
GLFW window + OpenGL 3.3 context, a fixed-timestep loop, input, a hand-written
math library, an API-agnostic renderer with an OpenGL backend (lit path,
debug lines, dynamic meshes, a screen-space HUD), a minimal `Scene`, a
`FirstPersonController`, Verlet rope physics, raycasting, and a retained-mode
`Hud`.

The game `games/02-strandbound` lets the player place anchors and tie / cut
ropes (drawn as solid textured tubes), with a HUD crosshair and readout. Its
level already contains a **home island** (20×20, at z=0) and a **far island**
(18×18, at z=−45) — both already solid colliders. The player starts on the
home island at z=7.

Two things are *not* yet real:
1. The `FirstPersonController` clamps the player to a flat ground plane at y=0
   **everywhere** — the player can walk straight across the gap on invisible
   ground.
2. A tied rope is decorative — the player cannot stand on or cross it.

M5 makes both real and adds a win condition.

Development stays game-driven: general capability goes in `engine/`, the
Strandbound-specific gameplay stays in the game.

## Scope

### In scope

- A **player state**: `Walking` or `Traversing` (on a rope), plus a `Won` end
  state.
- **Real footing** — a per-frame footing query against the island colliders;
  walking off solid ground respawns the player at the home start.
- A new game-side **`RopeWalker`** — the tightrope traversal: auto-mount at an
  anchor, a lean-meter balance challenge, movement along the rope, the
  traversal camera, fall / dismount / win.
- Engine: a **camera roll** angle (for the tightrope tilt) and
  **`Hud::setSize`** (for the lean meter).
- A HUD **lean meter** (shown while traversing) and a **win label**.

### Out of scope (deliberate)

- **A fall animation** — respawn is immediate (player chose "respawn player
  only"). No falling-body physics, no fade.
- **Bridge reset on failure** — a fall respawns only the player; placed
  anchors and tied ropes are kept.
- **A restart / replay flow** — on `Won` the demo is complete; re-running the
  executable restarts it.
- **Two-way rope physics** — the rope does not sag under the player's weight;
  it is the settled Verlet curve and the player rides its centerline.
- **Player-vs-prop collision** — the player still walks through the decorative
  prop boxes; only the two islands provide footing.
- **Jumping / vertical movement** — the player walks on flat island ground;
  the only vertical motion is the rope's sag while traversing.

## Design

### Player state

The game holds a `PlayerState` enum: `Walking`, `Traversing`, `Won`.

- **`Walking`** — the `FirstPersonController` drives the player as today.
- **`Traversing`** — the `FirstPersonController` is suspended; `RopeWalker`
  drives the camera. Entered by mounting a rope, left by dismounting, falling,
  or winning.
- **`Won`** — terminal; the win label shows, input still allows `Escape`.

### 1. Real footing and the gap

The flat infinite ground plane becomes real ground.

Each frame, while `Walking`, the game runs a **footing query**: a 2-D
point-in-AABB test of the player's XZ position against the **island
colliders** (the home and far island AABBs — a small named subset of the
level's colliders; the decorative props are excluded).

- **Footing found** → the player stands normally. Island tops are at y=0, so
  the `FirstPersonController`'s ground clamp stays at 0 — no controller change
  is needed for the common case.
- **No footing** (the player has walked over the gap or off any island edge)
  → the player is **respawned**: position reset to the home-island start
  (z=7), facing the far island. Per the approved design, only the player
  resets — placed anchors and tied ropes are untouched. The respawn is
  immediate; there is no fall animation.

The footing query is a pure function — `bool hasFooting(Vec2 playerXz, const
std::vector<Aabb>& islands)` — and is unit-tested.

### 2. Tightrope traversal — `RopeWalker`

`RopeWalker` is a new game-side class owning all traversal state and logic.
It is constructed with access to the rope list `RopeTool` owns, and is
steppable headlessly (no GL calls in its update) so its logic is unit-tested.

**Mounting.** While `Walking`, each frame `RopeWalker` checks whether the
player is standing within a small radius of a rope's **end anchor** (the first
or last point of a rope). If so, the player **auto-mounts**: `PlayerState`
becomes `Traversing`, the `FirstPersonController` is suspended, and the
traversal begins at that rope end (parameter `t` = 0 at the mounted end, 1 at
the far end).

**Movement along the rope.** The player has a parameter **`t` ∈ [0, 1]** along
the mounted rope. `W` advances `t` toward the far end, `S` retreats it; the
advance rate is a constant walk speed divided by the rope's length, so pace is
consistent regardless of rope length. The camera position is the rope
polyline sampled at `t` (linear interpolation between the two nearest rope
points) plus the eye height — so the player visibly dips through the rope's
sag. The mouse continues to control look direction (yaw / pitch) as in
`Walking`.

**The lean meter.** A signed scalar **`lean` ∈ [−1, +1]** represents balance:
0 is balanced, ±1 is a fall. On mounting, `lean` = 0. Each frame:

- `lean` **drifts** by a small amount whose magnitude grows with time spent on
  the current rope — early steps are forgiving, later steps demand attention.
  The drift's direction varies (a pseudo-random walk) so the player cannot
  predict it.
- `A` and `D` apply a **counter-steer** that pushes `lean` back toward 0.

The lean integration is expressed as a pure function — given the current
`lean`, a drift amount, a counter-steer input, and `dt`, it returns the new
`lean` — so it is unit-tested with deterministic inputs.

**Camera roll.** The traversal camera **rolls** (tilts about the view axis)
proportionally to `lean`: the horizon tips the direction the player is
losing balance. This is the primary felt cue.

**Falling off.** If `|lean|` reaches 1, the player **falls off the rope** →
respawn (player only; bridge kept), exactly like a gap fall.

**Dismounting.** Retreating to `t` = 0 (back onto the start anchor) dismounts
the player back to `Walking` at that anchor — no penalty.

**The rope** keeps its normal Verlet update during traversal. Pinned at both
anchors with no wind, it is effectively at rest, so sampling its points each
frame yields a stable curve; no special freeze is needed.

### 3. Winning

When a traversing player reaches **`t` = 1** *and the far end anchor lies on
the far island* (its XZ is inside the far-island footprint), the player
**auto-dismounts onto the far island** and `PlayerState` becomes `Won`.

Reaching `t` = 1 at an anchor that is **not** on the far island (for example a
rope tied between two home-island anchors) simply dismounts the player to
`Walking` at that anchor — no win.

On `Won`, the win HUD label is shown and the demo is complete.

### 4. The traversal camera

While `Traversing`, `RopeWalker` produces the view matrix instead of the
`FirstPersonController`: camera position is the sampled rope point + eye
height; yaw / pitch come from the mouse; **roll** comes from `lean`. The
engine's camera view-matrix construction gains a **roll angle** so this is a
clean engine capability rather than a hand-rolled matrix in the game.

### 5. HUD additions

Reusing the HUD subsystem:

- A **lean meter**, visible only while `Traversing`: a background "track"
  panel plus a "fill" panel whose width tracks `|lean|` (0 → empty, 1 → full).
  Its colour shifts from calm to danger as `|lean|` nears 1. Hidden in
  `Walking` and `Won`.
- A **win label** — a text element ("You crossed the gap!") shown on `Won`,
  hidden otherwise.
- The existing crosshair and anchor/rope readout remain.

The lean-meter fill panel must resize every frame, so the engine `Hud` gains
**`void setSize(HudId, Vec2)`** — a mutator alongside the existing
`setPosition` / `setColor` / `setVisible` (the HUD milestone's review already
flagged this as the expected M5 need).

### Engine additions (summary)

- **Camera roll** — the view-matrix path gains a roll angle.
- **`Hud::setSize(HudId, Vec2)`** — resize a retained HUD element.

Both are small, general, and reusable. Everything else is game-side.

### File layout

```
engine/scene/Camera.h, Camera.cpp (or FirstPersonController)  camera roll angle   (modified)
engine/ui/Hud.h, Hud.cpp                                       setSize mutator     (modified)
games/02-strandbound/RopeWalker.h, RopeWalker.cpp              traversal + lean    (new)
games/02-strandbound/RopeTool.h                                expose the rope list to RopeWalker (modified)
games/02-strandbound/main.cpp        player state, footing query, respawn, win, HUD wiring (modified)
tests/test_hud.cpp                                             setSize test        (modified)
tests/test_rope_walker.cpp           lean integration / t-advance / footing / state tests (new)
tests/CMakeLists.txt                                           register the test   (modified)
docs/engine/strandbound-m5.md  (or docs/game/...)              concept note        (new)
```

The exact engine file for the roll angle (`Camera` vs `FirstPersonController`)
is settled when the implementation plan is written, after reading those files.

## Testing

The pure / headless logic is unit-tested in the existing CTest harness:

- **Footing** — `hasFooting`: a point inside an island AABB's XZ footprint
  returns true; a point over the gap returns false; a point past an island
  edge returns false.
- **Lean integration** — the pure lean-update function: a counter-steer input
  opposing the drift reduces `|lean|`; with no counter-steer the drift moves
  `lean`; `lean` is reported as a fall when it reaches ±1.
- **t-advance** — advancing `t` is clamped to [0, 1]; the rate is normalized
  by rope length; forward and reverse inputs move `t` the expected way.
- **`RopeWalker` state transitions** — a headless step: mounting near an
  anchor enters `Traversing`; `|lean|` reaching 1 leaves `Traversing` (fall);
  reaching `t`=1 at a far-island anchor produces `Won`; reaching `t`=1 at a
  non-far anchor dismounts to `Walking`.
- **`Hud::setSize`** — changing an element's size changes the built quad's
  extent.

The camera roll, the on-screen lean meter, the feel of the balance challenge,
and the win screen are verified by running the game, as in every prior
milestone.

## Acceptance criteria

Launch `games/02-strandbound`. The player can walk the home island but
**falls (respawns at the start) on stepping over the gap or off any edge**.
The player places an anchor on the home island, aims across the gap to place
an anchor on the far island, and ties a rope between them. Walking into the
home anchor **mounts** the rope; the camera moves along the sagging rope as
`W` is held, the horizon **rolls** with the drifting lean, and a HUD **lean
meter** shows how close to falling the player is. Losing balance respawns the
player (the rope is still there to retry). Reaching the far island **wins** —
a "You crossed the gap!" label appears. `Escape` quits.

## Conventions

Unchanged from M1–M4: namespace `iron` for engine code; engine headers
included relative to `engine/`; `Mat4` column-major; C++23; CMake; commit
after every task with the `Co-Authored-By` trailer; MSVC multi-config tests
run with `ctest --test-dir build -C Debug --output-on-failure`. Work proceeds
on a feature branch; `main` is protected (PR + green CI required to merge).
