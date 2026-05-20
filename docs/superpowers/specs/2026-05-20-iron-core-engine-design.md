# Iron Core Engine — Design Spec

**Date:** 2026-05-20
**Status:** Approved
**Author:** Elias Hanken (with Claude)

## Purpose

Build a 3D game engine in C++ from scratch as a **learning project**. The
priority is understanding — the math, the rendering pipeline, the architecture
of an engine — not shipping a product. The engine is a reusable static library;
individual games branch off it.

A second deliverable runs alongside the code: an Obsidian-browsable set of
`.md` notes in `docs/` explaining the math and engine concepts as they are
implemented.

## Goals

- Hand-write the math library (vectors, matrices, quaternions) to learn it.
- A clean, API-agnostic renderer abstraction (RHI) with **one** backend
  implemented first (OpenGL 3.3+).
- Reach a verifiable first milestone: an orbitable, spinning, textured 3D cube.
- Keep the architecture ready for later systems: state stack, raycasting demo,
  simple multiplayer, lighting.
- Document the meaty math/architecture pieces as Obsidian notes.

## Non-Goals (YAGNI)

- No second graphics backend until the OpenGL path works end-to-end.
- No editor, no asset pipeline, no scripting language.
- No physics engine beyond what a specific game needs.
- No networking until there is a game that needs it.
- Not documenting every file — only concepts worth a note.

## Decisions

| Decision        | Choice                                              |
|-----------------|-----------------------------------------------------|
| Language        | C++ (C++20)                                         |
| Build system    | CMake 3.20+                                         |
| Compiler        | MSVC (Visual Studio Community 2026); no platform locks |
| Platform        | Windows-first; portable code so Linux/macOS stay open |
| Rendering       | RHI abstraction; OpenGL 3.3+ backend implemented first |
| Repo layout     | Monorepo — engine library + games in one repo       |
| Repo visibility | Public GitHub repo `iron-core-engine`               |
| Windowing/input | GLFW                                                |
| GL loader       | glad                                                |
| Image loading   | stb_image                                           |
| Math library    | Hand-written (no GLM)                               |

## Architecture

Monorepo built with CMake. The `engine/` tree compiles to a static library
`ironcore`. Each directory under `games/` is a separate executable that links
`ironcore`.

```
iron-core-engine/
  CMakeLists.txt        → top-level: pulls deps, adds engine + games
  engine/
    CMakeLists.txt      → builds static lib "ironcore"
    core/               → application loop, window, input, time, logging
    math/               → vec2/3/4, mat4, quaternion, transforms (hand-written)
    render/             → Renderer interface (RHI) + shared render types
      backends/opengl/  → OpenGLRenderer implementation
    scene/              → entity, transform component, mesh, camera
    state/              → game state stack (Menu / Playing / Paused)
  games/
    01-spinning-cube/   → first milestone; links ironcore
  docs/                 → Obsidian vault: math + engine concept notes
    math/
    engine/
    superpowers/specs/  → this spec
  third_party/          → GLFW, glad, stb_image (vendored or fetched)
  tests/                → unit tests for the math library
```

### Module responsibilities

- **core** — owns the application lifecycle. `Application` runs a
  fixed-timestep loop (update at a fixed dt, render as fast as possible).
  `Window` wraps GLFW. `Input` polls keyboard/mouse. `Time` tracks delta/total
  time. `Log` is a thin logging facade.
- **math** — pure, dependency-free value types: `Vec2/3/4`, `Mat4`,
  `Quaternion`, plus free functions for `translate`, `rotate`, `scale`,
  `lookAt`, `perspective`. This is the core learning surface and is unit-tested.
- **render** — the RHI. `Renderer` is an abstract interface: `clear()`,
  `beginFrame()`/`endFrame()`, `createMesh()`, `createTexture()`,
  `createShader()`, `draw(...)`. Backend-agnostic handle types. The
  `backends/opengl/` directory holds `OpenGLRenderer` and GL-specific resource
  wrappers.
- **scene** — `Entity` with a `Transform`, references a `Mesh` and material.
  `Camera` produces view + projection matrices. A `Scene` holds entities and a
  camera.
- **state** — a stack of `GameState` objects (`onEnter`, `onExit`, `update`,
  `render`). Lets a game push a pause menu over gameplay, etc.

### Data flow (one frame)

```
Application loop
  → Input.poll()
  → StateStack.update(dt)        (game logic, camera, entity transforms)
  → Renderer.beginFrame()
  → StateStack.render(renderer)  (entities submit draw calls via RHI)
  → Renderer.endFrame()          (swap buffers)
```

### Error handling

- Resource creation (shader compile, texture load) returns a result the caller
  must check; failures are logged with context and degrade gracefully where
  possible (e.g. fall back to a magenta "missing" texture).
- GLFW/GL initialization failures are fatal and abort with a clear message.
- The math library does not throw; it is value-only. Division-style operations
  document their preconditions.

## Build order (milestones)

1. **Project skeleton** — CMake, dependency wiring (GLFW/glad/stb_image), empty
   `ironcore` lib, empty `01-spinning-cube` executable that builds and runs.
2. **Window + loop + input** — GLFW window, fixed-timestep loop, keyboard/mouse
   input, clear screen to a color.
3. **Math library** — `Vec`, `Mat4`, `Quaternion`, transform/projection
   functions, with unit tests. Documented in `docs/math/`.
4. **RHI + OpenGL backend** — `Renderer` interface, `OpenGLRenderer`, shaders,
   vertex buffers, textures.
5. **Scene + camera → MILESTONE: spinning textured cube** — a textured cube
   rendered with model/view/projection matrices, orbitable camera.
6. *Later, each its own spec:* state stack wired into a game, raycasting demo
   game, simple UDP multiplayer, basic lighting.

## Testing strategy

- **Math library** — real unit tests (it is pure and deterministic). A
  lightweight assertion-based harness under `tests/`, run via CTest.
- **Rendering / scene** — verified visually against each milestone. The
  spinning-cube milestone is the acceptance check for the render path.

## Documentation strategy

Concept notes live in `docs/` as Markdown, browsable as an Obsidian vault.
A note is written **when the corresponding feature is implemented**, not before.
Notes cross-link with `[[wiki-links]]`. Coverage targets the meaty pieces, e.g.:

- `docs/math/vectors.md`, `docs/math/matrices.md`, `docs/math/quaternions.md`
- `docs/math/transforms-and-projection.md`
- `docs/engine/game-loop.md`, `docs/engine/render-pipeline.md`,
  `docs/engine/rhi-abstraction.md`

## Open questions

None. Ready for implementation planning.
