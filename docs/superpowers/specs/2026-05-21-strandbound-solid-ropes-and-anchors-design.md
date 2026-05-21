# Strandbound — "Solid Ropes & Anchors" — Design

**Status:** Approved 2026-05-21. Amended during execution — anchors are drawn
as **rings** (a torus), not cubes, after the plain-cube look tested poorly;
the rope tube emits `S + 1` vertices per ring (a duplicated seam vertex) so the
texture does not wrap backwards.

## Goal

A visual milestone: replace the 1-pixel debug-line ropes and anchor markers
with real geometry. Ropes become low-poly, **textured, lit tube meshes**;
anchors become solid lit **rings**. After this milestone the Strandbound rope
tool *looks* like a game, not a debug view.

## Context

Milestones M1–M4 are complete and merged to `main`. The engine has: a GLFW
window + OpenGL 3.3 context, a fixed-timestep loop, input, a hand-written math
library, an API-agnostic renderer (RHI) with an OpenGL backend (lit render
path + debug-line rendering), a minimal `Scene`, a `FirstPersonController`,
Verlet physics (`Rope`), and raycasting. The game `games/02-strandbound` lets
the player place anchors and tie / cut ropes — but ropes and anchors are drawn
with the **debug-line renderer** (1-pixel `GL_LINES`), so they look thin and
flat.

This milestone is interleaved before M5 ("bridge the gap"): a focused visual
upgrade. It is **not** an `Mx` gameplay milestone — no new player verbs.

Development stays game-driven: general capability goes in `engine/`, the
Strandbound-specific rope tool stays in the game.

## Scope

### In scope

- `updateMesh` on the renderer RHI — re-upload a mesh's geometry so a mesh
  handle can be refreshed every frame.
- `appendTube`, `appendBox`, and `appendTorus` — geometry builders that append
  a tube (around a polyline), a box, and a ring into a `MeshData`.
- `RopeTool` draws ropes as a combined textured tube mesh and anchors as a
  combined ring mesh, through the existing lit render path.
- A downloaded tiling rope texture, committed as a game asset.

### Out of scope (deliberate)

- The aim-marker reticle stays a debug-line cross — it is a transient pointer,
  and debug-draw is the right tool for it.
- No rope endcaps — rope ends sit at the anchor rings.
- No parallel-transport orientation frame — a simple per-segment frame is
  accepted (a hanging rope rarely hits the degenerate case).
- No per-resource `destroyMesh` API — the combined-mesh approach means cut
  ropes simply drop out of next frame's rebuild; no GPU resource is leaked.
- No anchor texturing — anchors are flat-colour ring markers (clarity over
  detail).
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

**`appendTorus`.** A pure builder that appends a ring into a `MeshData`:
`void appendTorus(MeshData& out, Vec3 center, float majorRadius, float minorRadius, int majorSegments, int minorSegments)`.
The ring lies in a fixed plane; at each step around the major circle it places
a minor circle of `minorSegments + 1` vertices, with outward normals, stitched
into a grid of triangles. Used for the anchor markers.

All three builders live in `engine/scene/Mesh.h` / `Mesh.cpp` alongside
`makeCube`, and are pure functions — unit-testable.

### 2. Game: RopeTool renders real geometry

`RopeTool` is given access to the renderer so it can own its own GPU
resources. Its constructor takes the renderer and the lit shader handle and
creates, once:
- `ropeTexture_` — the downloaded tiling rope texture, via `loadTexture`.
- `anchorTexture_` — a 1×1 solid bright-yellow texture, via `createTexture`
  (anchors are flat-colour markers; the lit shader is `texture × lighting`, so
  a 1×1 colour tints the whole ring).
- `ropesMesh_` and `anchorsMesh_` — two `MeshHandle`s, created from empty
  `MeshData` and refreshed every frame.

Each rendered frame, `RopeTool::draw`:
1. Builds one combined `MeshData` for the ropes — for every rope, `appendTube`
   from its `points()` — and calls `updateMesh(ropesMesh_, ...)`.
2. Builds one combined `MeshData` for the anchors — an `appendTorus` per
   anchor — and calls `updateMesh(anchorsMesh_, ...)`.
3. Submits both meshes through the existing lit `submit` path: `ropesMesh_`
   with the rope texture, `anchorsMesh_` with the yellow texture.
4. Queues the aim-marker cross via the debug-line API as before.

The per-rope / per-anchor debug-line drawing is removed. Cutting a rope just
means next frame's combined `ropesMesh_` is rebuilt without it — no GPU
resource to free. The combined-mesh rebuild happens once per render frame (in
`draw`), not per fixed step.

The rope texture is downloaded as a CC0 tiling rope/fibre image and committed
under `games/02-strandbound/assets/`; the game's existing `POST_BUILD`
asset-copy step already copies the whole `assets/` folder next to the
executable.

### File layout

```
engine/render/Renderer.h                            updateMesh added to the RHI (modified)
engine/render/backends/opengl/GLMesh.h/.cpp          dynamic buffers + update() (modified)
engine/render/backends/opengl/OpenGLRenderer.h/.cpp  updateMesh implementation (modified)
engine/scene/Mesh.h, Mesh.cpp                        appendTube + appendBox + appendTorus (modified)
games/02-strandbound/RopeTool.h, .cpp                textured tube + ring rendering (modified)
games/02-strandbound/main.cpp                        pass renderer + shader to RopeTool (modified)
games/02-strandbound/assets/                         a downloaded CC0 rope texture (new)
tests/test_mesh_builders.cpp                         appendTube / appendBox / appendTorus tests (new)
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
- `appendTorus`: the vertex and index counts match the segment counts; ring
  vertices sit within `majorRadius ± minorRadius` of the centre; degenerate
  segment counts append nothing.

The textured, lit visual result is verified by running the game, as earlier
milestones' rendering was.

## Acceptance criteria

Launch `games/02-strandbound`. Ropes are drawn as solid, rounded, **textured**
tubes that catch the directional light and visibly hang in a curve — not thin
lines. Anchors are solid lit yellow **rings**, clearly visible. Placing, tying,
and cutting all still work, and cut ropes disappear cleanly. The aim-marker
cross still works. `Escape` quits.

## Conventions

Unchanged from M1–M4: namespace `iron` for engine code; engine headers
included relative to `engine/`; `Mat4` column-major; C++23; CMake; commit after
every task with the `Co-Authored-By` trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`. Work proceeds on a
feature branch; `main` is protected (PR + green CI required to merge).
