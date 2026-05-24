# CC0 Asset Pack + Showcase Scene — Design

**Date:** 2026-05-24
**Milestone:** Visuals track #7 (final milestone in visuals track)
**Status:** Approved — proceeding to implementation plan

## Motivation

Previous milestones (multi-light, atmosphere, reflections, materials, normal+spec maps) built the visual capabilities of the engine. Two gaps remain:

1. **No real-quality textures.** Procedurally generated normal/spec maps proved the pipeline but were visually subtle to the point of invisibility (256×256 procedural wood map tiled 400× over a 20×20 island = noise, not grooves). Real CC0 PBR packs will make the normal/spec features actually visible.
2. **No single scene that demos everything.** Visual features are exercised across Strandbound at different times but no scene shows them all in one place. Hard to A/B-test future visual changes without one.

This milestone closes both gaps.

## Goals

- Ship a small library of real CC0 PBR textures in the repo, organized so any game can use them.
- Demonstrate every visual feature shipped to date (shadows, multi-light, fog, skybox, planar reflection, cubemap reflection, normal maps, specular, emission) in one dedicated scene.
- Add a reusable free-fly camera so the showcase (and future demos) can be inspected from any angle.

## Non-goals

- Strandbound is not retextured in this milestone (separate follow-up if desired).
- No sphere primitive — a cylinder doubles as the glossy reflector.
- No PBR (roughness/metalness/GGX) — Blinn-Phong is sufficient for showing off the maps.
- No asset hot-reload — load once at startup.

## Architecture

### Asset layout

```
assets/
  cc0/
    wood_planks/
      diffuse.png
      normal.png
      roughness.png
      CREDITS.txt
    metal_plate/
      ...
    red_brick/
      ...
    ground/
      ...
```

Each pack has 1k PNGs (≈1–3 MB per file). `CREDITS.txt` records the CC0 source URL and author.

Paths are resolved at runtime relative to the executable, matching the convention already used for `crate.jpg` / `rope.jpg` in Strandbound. A CMake step copies (or, on platforms that support it, symlinks) `assets/` next to each game's built exe.

### Asset acquisition

Done **at design time**, not at build time, by a one-shot PowerShell script (or interactive commands) that calls `Invoke-WebRequest` against Polyhaven's CDN:

```
https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/<name>/<name>_<map>_1k.png
```

Target asset names (subject to availability):
- Wood: `wood_planks_worn_03` (or `worn_planks`, `weathered_planks`)
- Metal: `metal_plate` (or `worn_metal_plate`, `rusty_metal`)
- Brick: `red_brick_03` (or `red_brick_diff_03`, `bricks_039`)
- Ground: `brown_mud_leaves_01` (or `forest_floor`, `aerial_grass_rock`)

If a primary candidate 404s, retry with the listed alternatives. If all four candidates for one slot fail, document the manual download URL in the plan and ask the user to download.

For each successful download we save three files (diffuse, normal, roughness) into the corresponding `assets/cc0/<pack>/` directory and write a `CREDITS.txt` with the Polyhaven URL and "CC0 — Polyhaven".

### Roughness → spec map conversion

Polyhaven publishes roughness maps where pixel value 1.0 = matte, 0.0 = mirror. The existing engine shader treats `specularMap` as a multiplier where bright = shiny. To avoid changing the shader (which is shared with the existing procedural metal spec path), we invert the roughness map **at load time**:

```cpp
namespace iron {
// Loads `path` via stb_image and returns RGBA bytes with R/G/B inverted
// (255 - x). Alpha is preserved as 255. Use when reading a Polyhaven
// roughness PNG that should be used with the engine's specular-intensity
// shader path.
std::vector<unsigned char> loadRoughnessAsSpec(const std::string& path,
                                                int& outWidth, int& outHeight);
}
```

This keeps the existing procedural metal spec map (`generateMetalSpecularMap`) working unchanged.

### Free-fly camera (`engine/scene/FreeFlyCamera.{h,cpp}`)

```cpp
namespace iron {
struct FreeFlyCamera {
    Vec3 position{0.0f, 2.0f, 5.0f};
    float yaw = 0.0f;     // radians; 0 = looking down -Z
    float pitch = 0.0f;   // radians; clamped to [-89°, +89°]
    float fovDeg = 60.0f;

    // Apply one frame of input. mouseDx/mouseDy are deltas in pixels.
    // Boolean flags are pressed-this-frame states; the helper integrates
    // movement at moveSpeed world units / second along the corresponding
    // local axes (W/S = forward/back along view, A/D = strafe, Q/E = down/up
    // in world Y).
    void update(float dt,
                float mouseDx, float mouseDy,
                bool fwd, bool back, bool left, bool right,
                bool worldDown, bool worldUp,
                float moveSpeed = 5.0f,
                float mouseSensitivity = 0.0025f);

    Mat4 viewMatrix() const;  // lookAt(position, position+forward(), worldUp)
    Vec3 forward() const;     // unit vector derived from yaw/pitch
};
} // namespace iron
```

Game code owns GLFW polling and feeds raw deltas in. Engine stays input-agnostic — no new input abstraction. Pitch is clamped to ±89° (just under straight up/down) to avoid gimbal flips. Mouse sensitivity is a parameter so a future demo can tune it.

### Showcase scene (`games/03-showcase/main.cpp`)

A single composed scene, no gameplay state, no win condition. Tour around with the camera.

**Layout** (world-space coordinates, +Y up):

| Prop | Position | Material |
|------|----------|----------|
| Ground plane, 40×40 | y=0 | ground pack |
| Wood crate stack (2 wide × 3 tall × 1 deep, 1.5m cubes) | left of center, x=-6 | wood pack |
| Brick wall, 8 × 6 × 0.5 | middle-back, z=-8 | brick pack |
| Metal cylinder (tube), r=0.8, h=4 | center, x=0 | metal pack, reflectivity=0.6 |
| Emissive box, 1×1×1 | hovering at (2, 5, 0), emissive=(2,2,2) | bare diffuse |
| Water pond plane, 16×16 | right of center, x=10, y=0.1 | reflectivity=0.5, planar reflection |
| Sun (directional) + shadow | overhead, slight angle | — |
| 3 colored point lights | red @(-6,3,0), green @(0,3,0), blue @(6,3,0) | — |
| Skybox | sunset cubemap (existing) | — |
| Fog | exponential, sunset-tinted (existing) | — |

Camera starts at `(8, 4, 12)` with yaw/pitch facing the origin. WASD horizontal, QE vertical, mouse rotates. ESC quits.

### Build wiring

- New `games/03-showcase/CMakeLists.txt` mirrors `games/02-strandbound/CMakeLists.txt`.
- Add `add_subdirectory(03-showcase)` to `games/CMakeLists.txt`.
- A CMake `add_custom_command(POST_BUILD ...)` or `file(COPY ...)` step ensures `assets/cc0/` is reachable from the showcase exe at runtime (mirroring how Strandbound finds `crate.jpg`).

## Data flow (one frame in the showcase)

1. Poll GLFW for keyboard + mouse → compute deltas.
2. `camera.update(dt, dx, dy, W, S, A, D, Q, E)`.
3. `renderer.beginFrame(...)` with `camera.viewMatrix()`, projection, sun/shadow params, fog, sky.
4. Submit each prop (ground, crates, wall, cylinder, emissive cube, water).
5. `renderer.endFrame()` — runs the 4-pass pipeline: shadow → planar reflection → lit → skybox.
6. Swap buffers.

## Testing

- `FreeFlyCamera::update` — yaw integrates from mouseDx with correct sign; pitch clamps at ±89°; W moves along `forward()`; A strafes perpendicular; Q goes down in world Y.
- `FreeFlyCamera::viewMatrix` — when position=origin, yaw=0, pitch=0, the matrix is `lookAt({0,0,0}, {0,0,-1}, {0,1,0})`.
- `loadRoughnessAsSpec` — a synthetic 2×2 PNG with known roughness values is loaded; output bytes equal `255 - input` per channel, alpha=255.

## Risks

- **WebFetch / Invoke-WebRequest failure** — Polyhaven's CDN may rate-limit, redirect, or change asset names. Mitigation: per-pack retry with alternatives; documented fallback (you download manually) if all candidates fail.
- **Repo size** — 4 packs × 3 PNGs × ~1–3 MB = ~15–25 MB of binary in git. Acceptable for now (no monorepo strain at this scale). If it grows further we add git-lfs.
- **Asset path resolution from IDE/build-dir** — Strandbound already solved this for `crate.jpg`; mirror its convention exactly to avoid surprises.

## Out of scope

- PBR upgrade (roughness/metalness/GGX shading) — separate future track.
- Sphere primitive — cylinder is sufficient as the glossy reflector.
- Asset hot-reload, asset manager abstraction — load-on-startup is fine.
- Retexturing Strandbound — separate follow-up.
- Input abstraction layer — game polls GLFW directly.
- glTF / model loader — boxes/tubes/quads are enough.
