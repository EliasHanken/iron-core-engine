# Iron Core Engine

A 3D game engine built from scratch in C++ as a learning project — the math,
the rendering pipeline, and engine architecture, all hand-written.

The engine core (`engine/`) builds a static library `ironcore`. Games under
`games/` link against it. Concept notes live in `docs/` and are browsable as an
Obsidian vault.

## What works today

- Windowing + OpenGL 3.3 context (GLFW + glad)
- Fixed-timestep game loop
- Keyboard / mouse input
- Hand-written math library: `Vec2/3/4`, `Mat4`, `Quaternion`, transform and
  projection builders — with unit tests
- API-agnostic renderer interface (RHI) with an OpenGL backend
- Orbit camera
- **Demo:** `games/01-spinning-cube` — a spinning, textured cube you can orbit

## Build

Requires CMake 3.20+ and a C++23 compiler (MSVC / Visual Studio 2026).
The `glad` dependency is generated at configure time and needs Python 3 with the
`jinja2` package installed (`pip install jinja2`).

```
cmake -S . -B build
cmake --build build
```

Run the demo:

```
build/games/01-spinning-cube/Debug/spinning-cube.exe
```

Controls: drag left mouse to orbit, `W`/`S` to zoom, `Escape` to quit.

Run the tests:

```
ctest --test-dir build -C Debug --output-on-failure
```

## Layout

```
engine/    static library "ironcore" (core, math, render, scene)
games/     executables that link the engine
tests/     unit tests for the math library
docs/      Obsidian vault: math + engine concept notes
```

## Roadmap

Next specs (each its own plan): game-state stack, a raycasting demo game,
simple UDP multiplayer, basic lighting.

## Design

See `docs/superpowers/specs/2026-05-20-iron-core-engine-design.md`.
