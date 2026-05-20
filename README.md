# Iron Core Engine

A 3D game engine built from scratch in C++ as a learning project — the math,
the rendering pipeline, and engine architecture, all hand-written.

The engine core (`engine/`) builds a static library `ironcore`. Games under
`games/` link against it. Concept notes live in `docs/` and are browsable as an
Obsidian vault.

## Build

Requires CMake 3.20+ and a C++23 compiler (MSVC / Visual Studio 2026).

```
cmake -S . -B build
cmake --build build
```

Run the first game:

```
build/games/01-spinning-cube/Debug/spinning-cube.exe
```

Run the tests:

```
ctest --test-dir build
```

## Design

See `docs/superpowers/specs/2026-05-20-iron-core-engine-design.md`.
