# Strandbound — "Solid Ropes & Anchors" — Design

**Status:** Approved 2026-05-21. Amended twice during execution:
1. The rope tube emits `S + 1` vertices per ring (a duplicated seam vertex) so
   the texture does not wrap backwards.
2. Anchor *shape* was iterated — solid cubes, then rings (a torus) — but both
   read as clutter next to the rope. The final call: **anchors stay as the
   crisp debug-line cross markers from M4.** The rope is the hero object; an
   anchor is just a point, and a thin cross marks a point cleanly. So this
   milestone makes the *rope* solid; the anchor marker is deliberately left
   as-is. (The torus builder added for the ring experiment was removed — no
   code uses it.)

## Goal

A visual milestone: replace the 1-pixel debug-line **ropes** with real
geometry — low-poly, **textured, lit tube meshes**. Anchor markers stay as
crisp debug-line crosses. After this milestone the Strandbound rope itself
*looks* like a game object, not a debug line.

## Context

Milestones M1–M4 are complete and merged to `main`. The engine has: a GLFW
window + OpenGL 3.3 context, a fixed-timestep loop, input, a hand-written math
library, an API-agnostic renderer (RHI) with an OpenGL backend (lit render
path + debug-line rendering), a minimal `Scene`, a `FirstPersonController`,
Verlet physics (`Rope`), and raycasting. The game `games/02-strandbound` lets
the player place anchors and tie / cut ropes — but ropes and anchors are drawn
with the **debug-line renderer** (1-pixel `GL_LINES`), so the rope looks thin
and flat.

This milestone is interleaved before M5 ("bridge the gap"): a focused visual
upgrade. It is **not** an `Mx` gameplay milestone — no new player verbs.

Development stays game-driven: general capability goes in `engine/`, the
Strandbound-specific rope tool stays in the game.

## Scope

### In scope

- `updateMesh` on the renderer RHI — re-upload a mesh's geometry so a mesh
  handle can be refreshed every frame.
- `appendTube` and `appendBox` — geometry builders that append a tube (around
  a polyline) and a box into a `MeshData`.
- `RopeTool` draws ropes as a combined textured tube mesh through the existing
  lit render path.
- A downloaded tiling rope texture, committed as a game asset.

### Out of scope (deliberate)

- Anchor markers stay debug-line crosses — a thin cross marks a point cleanly,
  and solid shapes (cube, ring) both tested as visual clutter. The aim-marker
  reticle likewise stays a debug-line cross.
- No rope endcaps — rope ends sit at the anchor points.
- No parallel-transport orientation frame — a simple per-segment frame is
  accepted (a hanging rope rarely hits the degenerate case).
- No per-resource `destroyMesh` API — the combined-mesh approach means cut
  ropes simply drop out of next frame's rebuild; no GPU resource is leaked.
- No new gameplay.

## Design

### 1. Engine: dynamic meshes and geometry builders

**`updateMesh`.** Add to the `Renderer` RHI:
`void updateMesh(MeshHandle mesh, const MeshData& data)`. It replaces a mesh's
vertex and index data. In the OpenGL backend, `GLMesh`'s vertex and index
buffers are created with `GL_DYNAMIC_DRAW` and `GLMesh` gains an
`update(const MeshData&)` that re-uploads both buffers (via `glBufferData`) and
refreshes the stored index count. `OpenGLRenderer::updateMesh` looks the handle
up and forwards to `GLMesh::update`. A mesh can now be created once (even from
empty data) and refreshed each frame.

**`appendTube`.** A pure builder that *appends* a tube around a polyline into a
`MeshData`:
`void appendTube(MeshData& out, const std::vector<Vec3>& points, float radius, int sides)`.
For each point it places a ring of `sides + 1` vertices (low-poly, ~6–8 sides;
the extra vertex duplicates the seam so the texture wraps cleanly) in the plane
perpendicular to the local rope direction, gives each an outward-pointing
normal and a UV (**U around the ring's circumference, V tiling along the
rope's length**), and stitches consecutive rings into triangles. Appending
(rather than returning) lets several ropes accumulate into one `MeshData`. The
per-point orientation frame is computed simply (a reference up vector and cross
products); full parallel transport is out of scope.

**`appendBox`.** A pure builder that appends a cube into a `MeshData`:
`void appendBox(MeshData& out, Vec3 center, Vec3 size)` — 24 vertices (4 per
face) with per-face normals and simple per-face UVs. `makeCube` is defined as
`appendBox` at the origin with side length 1.

Both builders live in `engine/scene/Mesh.h` / `Mesh.cpp` alongside `makeCube`,
and are pure functions — unit-testable.

### 2. Game: RopeTool renders the rope as real geometry

`RopeTool` is given access to the renderer so it can own its own GPU
resources. Its constructor takes the renderer and the lit shader handle and
creates, once:
- `ropeTexture_` — the downloaded tiling rope texture, via `loadTexture`.
- `ropesMesh_` — a `MeshHandle`, created from empty `MeshData` and refreshed
  every frame.

Each rendered frame, `RopeTool::draw`:
1. Builds one combined `MeshData` for the ropes — for every rope, `appendTube`
   from its `points()` — and calls `updateMesh(ropesMesh_, ...)`.
2. Submits `ropesMesh_` through the existing lit `submit` path with the rope
   texture.
3. Queues the anchor markers (a yellow three-axis cross at each anchor) and
   the aim-marker cross via the debug-line API.

The per-rope debug-line drawing is removed. Cutting a rope just means next
frame's combined `ropesMesh_` is rebuilt without it — no GPU resource to free.
The combined-mesh rebuild happens once per render frame (in `draw`), not per
fixed step.

The rope texture is downloaded as a CC0 tiling rope/fibre image and committed
under `games/02-strandbound/assets/`; the game's existing `POST_BUILD`
asset-copy step already copies the whole `assets/` folder next to the
executable.

### File layout

```
engine/render/Renderer.h                            updateMesh added to the RHI (modified)
engine/render/backends/opengl/GLMesh.h/.cpp          dynamic buffers + update() (modified)
engine/render/backends/opengl/OpenGLRenderer.h/.cpp  updateMesh implementation (modified)
engine/scene/Mesh.h, Mesh.cpp                        appendTube + appendBox (modified)
games/02-strandbound/RopeTool.h, .cpp                textured tube rendering (modified)
games/02-strandbound/main.cpp                        pass renderer + shader to RopeTool (modified)
games/02-strandbound/assets/                         a downloaded CC0 rope texture (new)
tests/test_mesh_builders.cpp                         appendTube / appendBox unit tests (new)
tests/CMakeLists.txt                                 register the new test (modified)
docs/engine/procedural-meshes.md                     concept note (new)
```

## Testing

Unit tests in the existing CTest harness for the pure builders:

- `appendBox`: appending one box yields 24 vertices and 36 indices; appending
  to a non-empty `MeshData` adds to it (indices are offset correctly so the
  second box references its own vertices); the box spans `center ± size/2`.
- `appendTube`: a tube over N points with S sides yields N×(S+1) vertices
  (each ring carries a duplicated seam vertex) and `(N-1)×S×6` indices; the
  ring vertices sit at `radius` from the polyline; vertex normals point
  outward; degenerate input (fewer than 2 points, fewer than 3 sides, or a
  non-positive radius) appends nothing.

The textured, lit visual result is verified by running the game, as earlier
milestones' rendering was.

## Acceptance criteria

Launch `games/02-strandbound`. Ropes are drawn as solid, rounded, **textured**
tubes that catch the directional light and visibly hang in a curve — not thin
lines. Anchors and the aim cursor remain crisp debug-line crosses. Placing,
tying, and cutting all still work, and cut ropes disappear cleanly. `Escape`
quits.

## Conventions

Unchanged from M1–M4: namespace `iron` for engine code; engine headers
included relative to `engine/`; `Mat4` column-major; C++23; CMake; commit after
every task with the `Co-Authored-By` trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`. Work proceeds on a
feature branch; `main` is protected (PR + green CI required to merge).
