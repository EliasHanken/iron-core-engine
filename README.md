# Iron Core Engine

A 3D game engine built from scratch in C++ as a learning project — the math,
the rendering pipeline, and engine architecture, all hand-written.

The engine core (`engine/`) builds a static library `ironcore`. Games under
`games/` link against it. Concept notes live in `docs/` and are browsable as an
Obsidian vault.

## Render backend

The engine has two backends behind one RHI: **Vulkan 1.3** (default, actively
developed) and **OpenGL 3.3** (deprecated, frozen — no new features, but
still compiles and runs the demos it shipped with). Select via CMake:

```
cmake -S . -B build                                  # Vulkan (default)
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl     # OpenGL (deprecated)
```

Vulkan demos: `01-spinning-cube`, `03-showcase`, `07-net-shooter`,
`08-particle-storm`. OpenGL-only demos (`02-strandbound`, `04-net-pingpong`,
`05-net-cubes`, `06-net-tag`) require `-DIRON_RENDER_BACKEND=opengl`.

## Build

Requires CMake 3.20+ and a C++23 compiler (MSVC / Visual Studio 2026).
The `glad` dependency is generated at configure time and needs Python 3 with the
`jinja2` package installed (`pip install jinja2`).

```
cmake -S . -B build
cmake --build build
```

Run a demo (Vulkan):

```
build/games/03-showcase/Debug/showcase.exe
```

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

Next track: physics overhaul (real character controllers, rigid bodies,
collision with rotation, joints). See
[`docs/superpowers/plans/`](docs/superpowers/plans/) for in-progress
specs and plans.

## Design

See `docs/superpowers/specs/2026-05-20-iron-core-engine-design.md`.
