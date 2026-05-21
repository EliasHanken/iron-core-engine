# Strandbound M4 — "Tie & Cut" — Design

**Status:** Approved 2026-05-21

## Goal

Build the fourth engine milestone toward the Strandbound mechanic demo: make
ropes **player-driven**. After M4 the player can place anchor points on
surfaces, tie ropes between anchors, and cut ropes — building and tearing down
a rope web in a first-person sandbox.

## Context

Milestones 1–3 are complete and merged to `main`. The engine currently has: a
GLFW window + OpenGL 3.3 context, a fixed-timestep loop, keyboard/mouse input,
a hand-written math library (`Vec`, `Mat4`, `Quaternion`, transforms), an
API-agnostic renderer (RHI) with an OpenGL backend (lit render path + debug-line
rendering), a minimal `Scene`, a `FirstPersonController`, an orbit `Camera`, a
cube mesh factory, and a Verlet physics module (`VerletPoint`,
`DistanceConstraint`, `Rope`). The game `games/02-strandbound` is a lit,
walkable floating island with one hardcoded rope that follows the player.

Development is **game-driven**: each milestone adds general-purpose capability
to `engine/`, driven by what the target game needs; the game in `games/` is
the forcing function and the test. General systems go in `engine/`;
game-specific code stays in the game. The longer-term goal is a reusable
engine for many games.

### M4 in the roadmap

The near-term target is a playable Strandbound mechanic demo (character +
ropes + bridge-a-gap). Milestones:

| # | Milestone | Player can |
|---|-----------|------------|
| M2 ✅ | A world to stand in | Walk around a lit floating island |
| M3 ✅ | Ropes that swing | Watch a rope dangle, drag, and swing |
| **M4** | **Tie & cut** | **Place anchors, tie ropes between them, cut ropes** |
| M5 | Bridge the gap | Cross the gap on a rope; the gap becomes real |

**This spec covers M4 only.** M5 is re-scoped once M4 ships.

The roadmap originally named M4 "tie, cut, pull." "Pull" is deferred: pulling
is only meaningful — and only testable — once ropes connect to objects or
structures that respond to being pulled, which is M5+ work. M4 delivers the
milestone's real outcome, "make and unmake ropes," with **tie and cut**.

## M4 Scope

### In scope

- **Raycasting math** in the engine: a `Ray` type, an `Aabb` type, and
  ray-vs-box / ray-vs-sphere intersection functions.
- A `FirstPersonController::aimRay()` accessor — the player's aim ray.
- A game-side `RopeTool` that owns anchors and ropes and implements the
  place / tie / cut interaction, plus its drawing.
- The `02-strandbound` game splits into `main.cpp` (setup + loop wiring) and
  `RopeTool.{h,cpp}`.

### Out of scope (later milestones — named to keep scope honest)

- A "pull" verb and objects that respond to being pulled — M5+.
- Rope severing into dangling halves — cut removes the whole rope.
- Rope-vs-world and rope-vs-player collision — ropes still pass through
  geometry. M5.
- Anchor removal — the player places anchors and ties/cuts ropes; un-placing
  an anchor is out.
- A HUD / crosshair / UI system — aim feedback is world-space debug-draw.
- Saving or loading the rope network.
- A general "physics world" with registered colliders — the engine provides
  ray-intersection *functions*; the game iterates its own handful of objects.

## Design

### 1. Engine: raycasting math

Pure geometry — no GLFW, no OpenGL — header-only and unit-testable, consistent
with the rest of `engine/math/`.

**`Ray`** (`engine/math/Ray.h`):

```cpp
struct Ray {
    Vec3 origin;
    Vec3 direction;  // expected to be unit length
};
```

**`Aabb`** (`engine/math/Aabb.h`) — an axis-aligned bounding box. The scene's
islands, props, and pole are all axis-aligned scaled cubes, so a box is the
right collider shape:

```cpp
struct Aabb {
    Vec3 min;
    Vec3 max;
};
```

**Intersection functions** (in `engine/math/Ray.h`). Each reports whether the
ray hits and, if so, the distance `t` along the ray to the nearest hit. A hit
must be in front of the ray origin (`t >= 0`):

- `bool intersectRayAabb(const Ray& ray, const Aabb& box, float& outT)` —
  the slab method. Used to find the surface point when placing an anchor.
- `bool intersectRaySphere(const Ray& ray, Vec3 center, float radius,
  float& outT)` — used to pick **anchors** (treated as spheres) and to pick
  **ropes** for cutting (each rope point is treated as a small sphere; the
  points are dense enough that aiming near the rope registers, and it reuses
  the same function).

The "report hit + out-distance" shape lets the game pick the *nearest* hit
across many candidates.

**`FirstPersonController::aimRay()`** — a new const accessor returning
`Ray{ eyePosition(), <look direction> }`. The look direction is the
controller's existing forward vector. This is the player's aim and is reusable
by any first-person game.

That is the entire engine surface for M4. The tie/cut/place interaction is
Strandbound's rope tool — game-specific — so it lives in the game.

### 2. Game: the `RopeTool`

The `02-strandbound` game splits into two translation units:
- `main.cpp` — window/renderer/scene setup, builds the static collider list,
  creates the `FirstPersonController` and the `RopeTool`, and runs the loop
  (the update/render lambdas forward to those two objects).
- `RopeTool.{h,cpp}` — the interaction.

**`RopeTool` owns:**
- `std::vector<Vec3> anchors` — placed anchor positions.
- `std::vector<Rope> ropes` — completed ropes, each spanning two anchors.
- The interaction state: *idle*, or *tying* — recorded simply as the
  start-anchor index (or -1 when idle). A `Rope` is only created once *both*
  anchors are known; while tying, the in-progress connection is shown as a
  plain guide line, not a simulated rope.
- The static scene colliders — a `std::vector<Aabb>` handed in by the game at
  construction, used for surface-placement raycasts.

**Per fixed step — an `update` taking the player's aim ray, the player
position, the per-step input edges, and `dt`:**
- **Place** (right-click edge): raycast the aim ray against the scene
  `Aabb`s; at the nearest hit, append a new anchor at the hit point.
- **Tie** (left-click edge): raycast against the anchor spheres for the
  nearest anchor. If *idle*, begin *tying* — just record the start-anchor
  index. If already *tying* and a *different* anchor is hit, create a `Rope`
  spanning the two anchors (its natural length is the anchor distance times a
  slack factor, so it dangles) and append it to `ropes`; return to *idle*.
  Tying onto the start anchor again, or onto nothing, is a no-op.
- **Cut** (C-key edge): raycast against every rope's points; if a rope is
  hit, erase it.
- Advance physics: `update` every completed rope. (Completed ropes have both
  endpoints pinned at fixed anchor positions, so they simply hang — M3
  physics. There is no in-progress rope to simulate.)

**`draw` (called in render):**
- Each anchor as a small debug-drawn marker (a short three-axis cross).
- Every completed rope as debug lines, one per segment (reusing M3's rope
  drawing).
- While tying, a straight guide line from the start anchor to the player, so
  the player sees the pending connection.
- An **aim indicator**: a marker at the current raycast hit point, colored by
  what the current aim would act on (surface / anchor / rope / nothing). This
  is world-space debug-draw — M4 needs no HUD.

Input edges drive the verbs: each of place / tie / cut must fire once per
click or key-press, not every step the button is held. `Input` already has a
`keyPressed` edge query (used for `C` / Cut) but only a level-state
`mouseButtonDown` for mouse buttons. M4 adds a `mouseButtonPressed` edge query
to `Input` — tracking previous/current mouse-button state across steps, the
same way `keyPressed` already works for keys. This is a small, general `Input`
addition.

### File layout

```
engine/math/Ray.h                      Ray, intersectRayAabb, intersectRaySphere (new)
engine/math/Aabb.h                     Aabb (new)
engine/scene/FirstPersonController.h/.cpp  aimRay() accessor (modified)
engine/core/Input.h/.cpp               mouseButtonPressed edge query (modified)
games/02-strandbound/RopeTool.h, .cpp  anchors, ropes, place/tie/cut, drawing (new)
games/02-strandbound/main.cpp          split: setup + wiring only (modified)
games/02-strandbound/CMakeLists.txt    add RopeTool.cpp to the executable (modified)
tests/test_ray.cpp                     ray-intersection unit tests (new)
tests/CMakeLists.txt                   register the new test (modified)
docs/engine/raycasting.md              concept note (new)
```

## Testing

Unit tests in the existing CTest harness, covering the pure ray math:

- `intersectRayAabb`: a ray hitting a box in front (correct `t`); a ray
  pointing away (miss); a ray that starts inside the box; a ray parallel to
  the box that misses; a ray that misses to the side.
- `intersectRaySphere`: a direct hit (correct `t`); a miss; a ray pointing
  away from a sphere behind it (miss); a near-tangent hit; a ray whose origin
  is inside the sphere.

The `RopeTool` interaction (place / tie / cut state machine) is game logic
verified by running the game — the same engine-tested / game-visual split used
in M2 and M3.

## Acceptance criteria

Launch `games/02-strandbound`. Aiming at a surface and right-clicking places
an anchor there. Aiming at one anchor and left-clicking, then walking to
another anchor and left-clicking, creates a rope that spans the two anchors
and hangs under gravity. Aiming at a rope and pressing `C` removes it. A
world-space aim indicator shows what the current aim is targeting. The player
can build a small rope web across the island and tear it down again. `Escape`
still quits.

## Conventions

Unchanged from M1–M3: namespace `iron`; engine headers included relative to
`engine/`; `Mat4` column-major; C++23; CMake; the simulation runs in the
fixed-timestep update; commit after every task with the `Co-Authored-By`
trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`.
