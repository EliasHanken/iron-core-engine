# Strandbound M3 — "Ropes that Swing" — Design

**Status:** Approved 2026-05-21

## Goal

Build the third engine milestone toward the Strandbound mechanic demo: a
**Verlet rope** that hangs in the world and swings, plus **debug-line
rendering** to draw it. After M3 the Strandbound game has a rope hanging from a
pole; one end follows the player, so walking around drags the rope and you
watch it swing, pull taut, and go slack in real time.

## Context

Milestones 1 (spinning cube) and 2 ("a world to stand in") are complete and
merged to `main`. The engine currently has: a GLFW window + OpenGL 3.3
context, a fixed-timestep loop, keyboard/mouse input, a hand-written math
library (`Vec`, `Mat4`, `Quaternion`, transforms), an API-agnostic renderer
(RHI) with an OpenGL backend including a lit render path, a minimal `Scene`, a
`FirstPersonController`, an orbit `Camera`, and a cube mesh factory. The game
`games/02-strandbound` is a lit, walkable floating island.

Development is **game-driven**: each milestone adds general-purpose capability
to `engine/`, driven by what the target game needs; the game in `games/` is
the forcing function and the test. The longer-term goal is a reusable engine
that can power many different games — so engine systems are designed general,
not hard-wired to Strandbound.

### M3 in the roadmap

The near-term target is a playable Strandbound mechanic demo (character +
ropes + bridge-a-gap). Milestones:

| # | Milestone | Player can |
|---|-----------|------------|
| M2 ✅ | A world to stand in | Walk around a lit floating island |
| **M3** | **Ropes that swing** | **Watch a rope dangle, drag, and swing** |
| M4 | Tie, cut, pull | Make and unmake ropes |
| M5 | Bridge the gap | Cross the gap on a rope; the gap becomes real |

**This spec covers M3 only.** M4 and M5 are re-scoped once M3 ships.

## M3 Scope

### In scope

- A small, general **Verlet physics module** in `engine/physics/`: a point
  type, a distance constraint, and the integration/constraint-satisfaction
  primitives.
- A **`Rope`** helper built on those primitives.
- **Debug-line rendering** added to the renderer (RHI + OpenGL backend).
- The `02-strandbound` game gets a fixed pole and one rope whose free end
  follows the player; the rope is drawn with debug lines.

### Out of scope (later milestones — named to keep scope honest)

- Rope-vs-world collision — the rope passes through islands and props. M4/M5.
- Rope self-collision.
- Tie / cut / pull — M3 has one hardcoded rope; player-driven rope creation
  is M4.
- A textured rope tube mesh — M3 draws the rope as debug lines; a real mesh
  is later polish.
- Player-vs-rope collision (standing on the rope) — M5.
- A general standalone constraint solver shared by cloth/chains/etc. — the
  reusable *primitives* are built now; a shared `solve()` is extracted only
  when a second consumer actually exists (YAGNI).

## Design

### 1. The Verlet physics module (`engine/physics/`)

Three pieces, all pure math — no GLFW, no OpenGL — so all unit-testable.

**`VerletPoint`** — a mass point with no explicit velocity:

```cpp
struct VerletPoint {
    Vec3 position;
    Vec3 previousPosition;
    bool pinned = false;
};
```

Velocity is implicit in `position - previousPosition`. The integration step,
for each **non-pinned** point:

```
next = position + (position - previousPosition) + acceleration * dt * dt
previousPosition = position
position = next
```

`acceleration` is gravity. Pinned points are skipped entirely — they are
anchors, moved only by external code.

**`DistanceConstraint`** — keeps two points a fixed distance apart:

```cpp
struct DistanceConstraint {
    int a;             // index of the first point
    int b;             // index of the second point
    float restLength;
};
```

Satisfying it: measure the current distance between the two points, compute
the error against `restLength`, and move the points to correct it. Each point
moves **half** the correction — unless one is pinned, in which case the free
point moves the **full** correction and the pinned point stays put. (If both
are pinned, the constraint does nothing.) A degenerate zero-length separation
is guarded against.

**Solver step.** For M3 the solver loop lives in `Rope::update`: integrate all
points once, then satisfy every constraint **K times** in a loop (more
iterations → stiffer, less stretchy rope; the default is around 16). Running
inside the fixed-timestep update makes the simulation deterministic. A
standalone reusable `solve(points, constraints, ...)` is intentionally **not**
extracted yet — the reusable atoms (`VerletPoint` + integration,
`DistanceConstraint` + satisfaction) are enough until a second consumer (e.g.
cloth) exists.

**`Rope`** (`engine/physics/Rope.h` / `Rope.cpp`) — the thin helper built on
the primitives:

- Constructed with two endpoint positions and a segment count `N`. It creates
  `N + 1` `VerletPoint`s spaced evenly along the line between the endpoints,
  and `N` `DistanceConstraint`s linking consecutive points. Each segment's
  `restLength` is the natural (unstretched) segment length.
- The two endpoint points (index `0` and index `N`) are **pinned**.
- `setEndpointA(Vec3)` / `setEndpointB(Vec3)` reposition the pinned endpoints
  — called each frame (one follows the player, one stays on the pole).
- `update(float dt)` runs the solver: integrate under gravity, re-pin the
  endpoints, satisfy constraints `K` times.
- Exposes read access to the point positions so the renderer can draw the
  rope.

### 2. Debug-line rendering

A general capability on the `Renderer` RHI interface — backend-agnostic and
useful well beyond M3 (physics, AI paths, bounding volumes, coordinate axes):

- **`drawLine(Vec3 a, Vec3 b, Vec3 color)`** — queues one colored 3D line
  segment for the current frame.
- **`flushDebugLines(const Mat4& view, const Mat4& projection)`** — renders
  every queued segment and clears the queue. The game calls this once per
  frame, after submitting the scene objects and before `endFrame`.

These are added as pure-virtual methods on `Renderer`; they are purely
additive, so the existing `01-spinning-cube` game is unaffected.

**OpenGL backend** — a `GLDebugLines` helper class (`engine/render/backends/
opengl/GLDebugLines.h` / `.cpp`), in the same style as the existing
`GLMesh` / `GLShader` / `GLTexture` wrappers. It owns:
- a **dynamically-updated** vertex buffer (the line set is rebuilt every
  frame) — uploaded with `GL_DYNAMIC_DRAW`;
- a tiny flat-color shader (position → MVP, output a per-vertex color);
- and draws with `GL_LINES`.

Lines are **depth-tested**, so a rope segment is correctly occluded by an
island rather than drawn on top of it. `OpenGLRenderer` owns a `GLDebugLines`
instance, accumulates segments from `drawLine`, and renders them in
`flushDebugLines`.

### 3. The game — a rope you drag around

`games/02-strandbound/main.cpp` is extended:

- Add a **pole** to the scene — a tall, thin box (a scaled cube) standing on
  the island. The top of the pole is the rope's fixed anchor.
- Create one `Rope` from the pole top to the player's starting position, with
  a sensible segment count (around 20).
- Each fixed-step update: set the rope's fixed endpoint at the pole top, set
  its movable endpoint at a point on the player (around waist height —
  `player.position()` plus a small vertical offset), then call
  `rope.update(dt)` with the fixed timestep.
- Each render: after submitting the scene objects, queue one `drawLine` per
  rope segment (connecting consecutive `VerletPoint`s) in a rope color, then
  call `flushDebugLines(view, projection)`.

Walking around drags the movable endpoint, and the rope swings, pulls taut,
and goes slack in response.

### File layout

```
engine/physics/VerletPoint.h                     VerletPoint + integration (new)
engine/physics/DistanceConstraint.h              DistanceConstraint + satisfy (new)
engine/physics/Rope.h, Rope.cpp                  Rope helper (new)
engine/render/Renderer.h                         drawLine + flushDebugLines (modified)
engine/render/backends/opengl/GLDebugLines.h/.cpp dynamic line buffer + shader (new)
engine/render/backends/opengl/OpenGLRenderer.h/.cpp  debug-line implementation (modified)
engine/CMakeLists.txt                            new sources (modified)
games/02-strandbound/main.cpp                    pole + rope + debug-line draw (modified)
tests/test_verlet.cpp                            physics unit tests (new)
tests/CMakeLists.txt                             register the new test (modified)
docs/engine/rope-physics.md                      concept note (new)
```

## Testing

Unit tests in the existing CTest harness, covering the pure physics:

- A free `VerletPoint` under gravity falls the distance Verlet integration
  predicts after a known number of steps; a point at rest with no acceleration
  stays put.
- A `pinned` point never moves under integration.
- A `DistanceConstraint` whose points are too far apart pulls them back toward
  `restLength`; one too close pushes them apart.
- A constraint with one pinned endpoint moves only the free point, and by the
  full correction.
- Iterating a constraint repeatedly converges the distance to `restLength`.

The rope *swinging* is inherently visual and is verified by running the game,
as M2's lighting was.

## Acceptance criteria

Launch `games/02-strandbound`. A rope hangs from a pole on the island, drawn
as a connected line. As the player walks around, the rope's free end follows
them: the rope swings, drags behind, pulls taut when the player moves away
from the pole, and goes slack when they move closer. The motion is smooth and
stable — the rope does not jitter apart, explode, or stretch without limit.
`Escape` still quits.

## Conventions

Unchanged from M1/M2: namespace `iron`; engine headers included relative to
`engine/`; `Mat4` column-major; C++23; CMake; the simulation runs in the
fixed-timestep update; commit after every task with the `Co-Authored-By`
trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`.
