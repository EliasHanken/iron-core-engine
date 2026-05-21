# Strandbound M2 — "A World to Stand In" — Design

**Status:** Approved 2026-05-21

## Goal

Build the second engine milestone toward a playable **Strandbound** mechanic
demo: a lit, first-person world the player can walk around. After M2 you can
launch the Strandbound game and explore a floating island in first person,
with directional lighting and gravity.

## Context

Milestone 1 (the spinning textured cube) is complete and merged to `main`. The
engine currently has: a GLFW window + OpenGL 3.3 context, a fixed-timestep
loop, keyboard/mouse input, a hand-written math library (`Vec`, `Mat4`,
`Quaternion`, transform/projection builders), an API-agnostic renderer (RHI)
with an OpenGL backend (shader/mesh/texture wrappers), an orbit `Camera`, and a
cube mesh factory.

The agreed development model is **game-driven**: the engine grows
general-purpose capabilities in `engine/`, driven by what a concrete target
game needs; the game in `games/` is the forcing function and the test.

### The Strandbound preview roadmap

The near-term target is a **simple playable mechanic demo** of Strandbound:
a first-person character on a floating island who can tie, cut, and pull ropes
to bridge a gap to a second island. No enemies, survival, or crafting.

That target decomposes into four engine milestones, each its own spec → plan →
build cycle:

| # | Milestone | Adds | Player can |
|---|-----------|------|------------|
| **M2** | A world to stand in | Lighting, a minimal `Scene`, a first-person controller, a ground clamp | Walk around a lit floating island |
| **M3** | Ropes that swing | Debug-line rendering, Verlet rope physics (mass points + distance constraints) | Watch a rope dangle and swing |
| **M4** | Tie, cut, pull | An interaction system, collision queries | Make and unmake ropes |
| **M5** | Bridge the gap | A second island, walkable ropes, a win condition | Play the demo: cross the gap |

**This spec covers M2 only.** M3–M5 are re-scoped with real knowledge once the
preceding milestone ships.

## M2 Scope

### In scope

- A directional-light + ambient lighting model in the renderer.
- A minimal engine `Scene` type (a list of render objects + the light).
- A `FirstPersonController` (movement, mouse-look, gravity, ground clamp).
- Cursor capture for mouse-look.
- A new game executable, `games/02-strandbound`, that assembles a floating
  island from scaled cubes and lets the player walk around it.

### Out of scope (later milestones — named to keep scope honest)

- Ropes and any physics/constraint module — M3.
- Real collision (raycasts vs geometry) — only a flat-island ground clamp
  exists in M2.
- Interaction / tie / cut / pull — M4.
- Enemies, survival, crafting — out of the preview entirely.
- Character model and animation — the demo is first-person.
- A scene editor — explicitly ruled out for the project.

## Design

### 1. Lighting

A single **directional light** plus an **ambient** term, evaluated per fragment
as Lambert diffuse and modulated by the object's texture:

```
litColor = textureColor * (ambient + max(dot(N, -lightDir), 0) * lightColor)
```

- The `Vertex` struct already carries a `normal`, so no mesh changes are
  needed.
- The vertex shader transforms the normal by `mat3(uModel)`. This is correct
  for uniform scaling, which is all M2 uses (the island and decorations are
  uniformly scaled cubes). A proper inverse-transpose normal matrix can be
  added in a later milestone if non-uniform scaling appears. This assumption
  is documented in the shader and the concept note.
- New type `DirectionalLight { Vec3 direction; Vec3 color; float ambient; }`
  in the engine.
- The renderer uploads light uniforms (`uLightDir`, `uLightColor`,
  `uAmbient`) the same way it already uploads the MVP matrices. `beginFrame`
  grows a `DirectionalLight` parameter (alongside the existing clear color);
  `submit` sets the light uniforms on the bound shader.
- The lit GLSL shader is provided by the game (as the spinning-cube game
  provided its shader). The engine renderer only guarantees that the light
  uniforms are set if the shader declares them.

### 2. Minimal engine `Scene`

New header `engine/scene/Scene.h`:

```cpp
struct RenderObject {
    Mat4          transform = Mat4::identity();
    MeshHandle    mesh      = kInvalidHandle;
    TextureHandle texture   = kInvalidHandle;
};

struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight          light;
};
```

The game fills a `Scene`. The render path iterates `scene.objects` and issues
one `submit()` per object. This is a struct and a vector — deliberately not an
ECS. It can evolve into a component system in a future milestone only if one
genuinely demands it.

`DirectionalLight` lives in its own small header (e.g.
`engine/render/Light.h`) so both `Scene.h` and the renderer can include it
without a dependency cycle.

### 3. `FirstPersonController`

New unit `engine/scene/FirstPersonController.{h,cpp}`. It owns the player and,
for a first-person camera, *is* the camera — position and orientation in one
place.

State:
- `Vec3 position` (the player's feet / base position)
- `float yaw, pitch` (radians; pitch clamped away from straight up/down)
- `Vec3 velocity` (for gravity)

Per fixed update step it:
- reads mouse delta → adjusts `yaw`/`pitch`;
- reads WASD → moves horizontally along the yaw-facing direction;
- applies gravity to vertical velocity;
- clamps the player's feet to a configurable ground height (M2's stand-in for
  collision — the island is a flat-topped box at a known height).

It exposes:
- `Mat4 viewMatrix() const` — built from position + yaw/pitch via `lookAt`;
- `Vec3 eyePosition() const` — position plus an eye-height offset;
- setters for ground height, eye height, move speed, mouse sensitivity.

The controller takes per-step input and the frame delta; it does not own the
`Input` object.

### 4. Cursor capture

Mouse-look needs the OS cursor hidden and locked to the window. Add a small
method to `Window` (or `Input`) wrapping
`glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED)`, and the inverse
for a normal cursor. The Strandbound game captures the cursor on start;
`Escape` still quits.

### 5. The game — `games/02-strandbound`

A new executable; the spinning cube stays as a finished M1 artifact.

M2 game content (all game-side, no engine mesh code):
- A **floating island**: a wide, flat-topped box — the existing `makeCube()`
  mesh scaled via its `RenderObject` transform.
- A handful of **decoration objects**: a few smaller boxes on the island, so
  the `Scene` holds multiple objects and the lighting shades varied surfaces.
- A **second island** across a gap, in the distance — visual only in M2, to
  foreshadow the M5 goal.
- The game provides the **lit shader** GLSL, builds the `Scene`, creates and
  drives the `FirstPersonController`, and runs the loop: update controller,
  then render every `Scene` object.

The game resolves its texture asset via `iron::executableDir()` (the
executable-relative path helper added after M1).

### File layout

```
engine/render/Light.h                         DirectionalLight (new)
engine/scene/Scene.h                           Scene + RenderObject (new)
engine/scene/FirstPersonController.h/.cpp      first-person player (new)
engine/render/Renderer.h                       beginFrame gains a light param (modified)
engine/render/backends/opengl/OpenGLRenderer.* light uniform upload (modified)
engine/core/Window.h/.cpp                      cursor capture (modified)
games/02-strandbound/CMakeLists.txt            new executable (new)
games/02-strandbound/main.cpp                  the M2 game (new)
games/02-strandbound/assets/                   texture(s) for the island (new)
tests/test_first_person_controller.cpp         controller unit tests (new)
docs/engine/lighting.md                        concept note (new)
```

## Testing

- Unit tests in the existing CTest harness for `FirstPersonController`:
  - facing −Z, "forward" decreases Z; "right" increases X; etc.;
  - yaw/pitch produce the expected view matrix / look direction;
  - gravity plus the ground clamp leaves the player resting exactly at ground
    height (does not sink through, does not float).
- Lighting is inherently visual and is verified by running the game and
  observing it, as the cube's texturing was.

## Acceptance criteria

Launch `games/02-strandbound`. With WASD + mouse-look the player walks around a
lit floating island in first person. Objects are visibly shaded by the
directional light — lit faces are brighter than faces turned away, with a
non-black ambient floor. Gravity holds the player on the island; the player
cannot fall through it. `Escape` quits.

## Conventions

Unchanged from M1: namespace `iron`; engine headers included relative to
`engine/`; `Mat4` column-major; C++23; CMake; commit after every task with the
`Co-Authored-By` trailer; MSVC multi-config tests run with
`ctest --test-dir build -C Debug --output-on-failure`.
