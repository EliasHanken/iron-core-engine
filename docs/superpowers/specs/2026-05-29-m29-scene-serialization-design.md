# M29 — Scene Serialization + Sandbox Runtime (Design)

**Date:** 2026-05-29
**Status:** Approved
**Predecessors:** M22-M24 (glTF + animation), M26 (audio), M28 (hot-reload)
**Successors:** M30 (editor UI: ImGui scene inspector) → M31 (gizmos + placement) → collision/audio authoring. This is the first milestone of the **editor track**.

## Goal

Define a JSON scene file format (renderable entities + lighting), a load/save library, and a minimal `games/11-sandbox` runtime that loads a scene file and renders it with a free-fly camera. This is the data foundation the editor authors in later milestones — UI comes next; this milestone is format + loader only.

## Context & Motivation

The existing `iron::Scene` / `RenderObject` (`engine/scene/Scene.h`) is a legacy M2-era abstraction used only by `games/02-strandbound`. The games that matter now (net-shooter) build draw calls imperatively and generate geometry procedurally (`Arena::buildArena` → AABBs). There is no current scene model representing how the engine actually works, and no way to author or persist a level outside C++. The editor track needs a serializable scene model; M29 defines it fresh rather than retrofitting the legacy struct.

**Why a new sandbox (not retrofit net-shooter):** net-shooter's geometry is tangled with networked determinism (host + clients must agree on procedural geometry) and imperative multiplayer logic. A clean `games/11-sandbox` proves load → render without that complexity. net-shooter migration is explicitly deferred (it remains the multiplayer proving ground; the editor is for authoring new content).

## Non-Goals

- **Editor UI / gizmos** — M30+. This milestone has no interactive editing; it loads and renders.
- **Collision shapes, audio emitters** — the format extends to carry these when the editor can author them (M31+). Not in v1.
- **Skinned / animated entities** — static meshes only. Animated characters in scenes is a later concern.
- **Skybox** — net-shooter's sky is generated procedurally in code, and cubemap-from-disk loading is its own task. v1 uses a solid `clearColor`. Skybox is a future format extension.
- **Sphere primitive** — the engine has `makeCube` + `appendQuad` (plane) but no sphere builder. v1 primitives are Cube + Plane; Sphere is a trivial future add.
- **net-shooter migration** — deferred, possibly indefinitely.

## Architecture — three units

### 1. `engine/scene/SceneFormat.h` — serializable data types

The crux of the design: the scene file references assets by **path / primitive name**, never by runtime GPU handle. A separate loader resolves those references to handles. This keeps the format pure data — serializable, diffable, and independent of any live renderer.

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"

#include <optional>
#include <string>
#include <vector>

namespace iron {

// A builtin procedural mesh. v1 supports the two the engine already has
// builders for (makeCube / appendQuad). Sphere etc. are future additions.
enum class PrimitiveKind { Cube, Plane };

// How an entity gets its geometry: either a builtin primitive OR a path to
// a static (non-skinned) glTF file. If `primitive` is set it wins; else
// `gltfPath` is loaded. If neither resolves, the loader logs and skips the
// entity (the scene still loads).
struct MeshRef {
    std::optional<PrimitiveKind> primitive;
    std::string                  gltfPath;
};

// Surface appearance. Texture paths resolve to engine textures at load;
// "" means "use the engine's builtin default" (white / flat-normal / no-spec).
struct MaterialDef {
    std::string albedoPath;
    std::string normalPath;
    std::string specularPath;
    Vec3        emissive     = {0.0f, 0.0f, 0.0f};
    float       uvScale      = 1.0f;
    float       reflectivity = 0.0f;
};

// One placed object: a transform + what to draw.
struct SceneEntity {
    std::string name;
    Vec3        position = {0.0f, 0.0f, 0.0f};
    Quat        rotation = Quat::identity();   // serialized as xyzw
    Vec3        scale    = {1.0f, 1.0f, 1.0f};
    MeshRef     mesh;
    MaterialDef material;
};

// A complete authored scene: placed entities + global lighting/environment.
struct SceneFile {
    std::vector<SceneEntity> entities;
    DirectionalLight         sun;                         // direction + color
    Vec3                     ambient    = {0.1f, 0.1f, 0.1f};
    std::vector<PointLight>  pointLights;
    Fog                      fog;                          // density 0 = off
    Vec3                     clearColor = {0.5f, 0.6f, 0.7f};
};

}  // namespace iron
```

### 2. `engine/scene/SceneIO.{h,cpp}` — JSON ↔ SceneFile

```cpp
// engine/scene/SceneIO.h
std::optional<SceneFile> loadSceneFile(const std::string& path);
bool                     saveSceneFile(const SceneFile& scene, const std::string& path);
```

Pure data ↔ JSON via vendored `nlohmann/json`. No GPU calls, so **headlessly unit-testable**. This is where the TDD lives:
- `loadSceneFile` returns `nullopt` on a missing file or malformed JSON (logs the error).
- Missing optional fields fall back to the struct defaults (a scene with just `entities` loads fine; `ambient`/`fog`/`clearColor` default).
- `saveSceneFile` writes pretty-printed JSON (2-space indent) so files are human-diffable.

JSON shape (illustrative):
```json
{
  "clearColor": [0.5, 0.6, 0.7],
  "ambient":    [0.1, 0.1, 0.1],
  "sun":   { "direction": [-0.4, -1.0, -0.3], "color": [1.0, 0.95, 0.9] },
  "fog":   { "color": [0.5, 0.6, 0.7], "density": 0.0 },
  "pointLights": [
    { "position": [0, 3, 0], "color": [1, 0.5, 0.2], "intensity": 2.0, "range": 12.0 }
  ],
  "entities": [
    {
      "name": "floor",
      "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [20, 1, 20],
      "mesh": { "primitive": "plane" },
      "material": { "uvScale": 8.0 }
    },
    {
      "name": "glow-cube",
      "position": [-2, 1, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1],
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.6, 0.2, 0.1] }
    },
    {
      "name": "helmet",
      "position": [2, 1.5, 0], "rotation": [0, 0, 0, 1], "scale": [1.5, 1.5, 1.5],
      "mesh": { "gltfPath": "assets/damaged-helmet/DamagedHelmet.gltf" },
      "material": {}
    }
  ]
}
```

To keep asset vendoring minimal, the demo's primitives use default (white / emissive-tinted) materials — no external texture PNGs need vendoring. Textured surfaces are demonstrated by the one static glTF (Damaged Helmet, CC0), which carries its own maps via the M22.5 material path. That glTF is vendored under `games/11-sandbox/assets/damaged-helmet/` (copy of the one `10-gltf-viewer` already ships).

(The exact field names for `DirectionalLight` / `PointLight` / `Fog` are matched to those structs' real members during implementation.)

### 3. `games/11-sandbox` — the loader/runtime

A minimal Vulkan game (mirrors `10-gltf-viewer`'s structure: free-fly camera, lit shader, HUD with a frame readout). It:

1. `loadSceneFile("assets/scenes/demo.json")`.
2. For each `SceneEntity`: resolve `MeshRef` → `MeshHandle` (build the primitive via `makeCube`/`appendQuad`, or `loadGltfModel` for a path), resolve `MaterialDef` → texture handles (`loadTexture` per non-empty path, else the engine defaults), compose the model matrix from position/rotation/scale, build a `DrawCall`.
3. `beginFrame` with the scene's sun/ambient/point-lights/fog/clearColor, submit all draw calls, `endFrame`.
4. Free-fly camera (WASD + mouse) to inspect.

Ships a hand-authored `assets/scenes/demo.json` (a floor plane + a few cubes + one static glTF) so there is something to load on first run. A mesh/texture path that fails to resolve logs a warning and that entity is skipped — the rest of the scene still renders.

## Dependency

Vendor `nlohmann/json` as a single header at `third_party/json/json.hpp` with an INTERFACE CMake target (mirrors the M28 `dr_libs` pattern). tinygltf bundles its own copy internally, but vendoring ours decouples the scene format from tinygltf's internals and keeps the include path explicit.

## Data Flow

```
Author (hand-edit JSON now; editor UI later)
   -> demo.json
   -> loadSceneFile()           [SceneIO]  -> SceneFile (pure data)
   -> per entity: resolve MeshRef + MaterialDef -> MeshHandle + TextureHandles  [sandbox]
   -> DrawCall per entity + scene lighting -> renderer.beginFrame/submit/endFrame
   -> rendered scene, free-fly camera

saveSceneFile(SceneFile)  -> pretty JSON  (used by tests now; by the editor later)
```

## Files Changed

**Create:**
- `engine/scene/SceneFormat.h` — the serializable data types.
- `engine/scene/SceneIO.h` / `SceneIO.cpp` — JSON load/save.
- `third_party/json/json.hpp` — vendored nlohmann/json single header.
- `third_party/json/CMakeLists.txt` — INTERFACE target `nlohmann_json`.
- `tests/test_scene_io.cpp` — round-trip + malformed + defaults tests.
- `games/11-sandbox/main.cpp` — the runtime.
- `games/11-sandbox/CMakeLists.txt` — target + asset copy (POST_BUILD copy of `assets/`, mirroring 10-gltf-viewer).
- `games/11-sandbox/assets/scenes/demo.json` — hand-authored demo scene.
- `games/11-sandbox/assets/damaged-helmet/*` — vendored static glTF (copy of 10-gltf-viewer's CC0 Damaged Helmet) for the demo's textured entity.
- `docs/engine/scene-format.md` — format reference doc.

**Modify:**
- `third_party/CMakeLists.txt` — `add_subdirectory(json)`.
- `engine/CMakeLists.txt` — add `scene/SceneIO.cpp`; link `nlohmann_json` PRIVATE.
- `tests/CMakeLists.txt` — register `test_scene_io`.
- `CMakeLists.txt` (root) — `add_subdirectory(games/11-sandbox)`.

**Unchanged (deliberately):** `engine/scene/Scene.h` (legacy; strandbound keeps using it).

## Test Plan

**Unit (`tests/test_scene_io.cpp`), headless:**
1. Round-trip: build a `SceneFile` in code (2 entities — one primitive, one glTF path; a sun; one point light; fog; custom clearColor), `saveSceneFile` to a temp path, `loadSceneFile` back, assert every field equal (floats within epsilon).
2. Malformed JSON (write garbage to a temp file) → `loadSceneFile` returns `nullopt`.
3. Missing file → `nullopt`.
4. Minimal scene (JSON with only `entities`, each with only a primitive) loads; omitted `ambient`/`fog`/`clearColor`/`sun` take struct defaults.
5. `MeshRef` round-trips both forms: primitive entity keeps its `PrimitiveKind`; glTF entity keeps its `gltfPath` and has no primitive.

**Visual (`games/11-sandbox`):**
- `sandbox.exe` loads `demo.json` → renders the floor plane, a few textured cubes, and one static glTF under the scene's sun + ambient + point light; free-fly around it.
- Edit `demo.json` (move an entity, change a color), relaunch → change reflected. (Live hot-reload of scenes is a future nicety; relaunch is fine for v1.)

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Round-trip float equality is brittle | Test compares with epsilon, not `==`. |
| nlohmann/json compile-time cost (large header) | Included only in `SceneIO.cpp` (one TU) + the test; not in the public header. |
| `Quat` serialized as xyzw but loaded mis-ordered | Round-trip test catches it; document the field order in the JSON and the format doc. |
| glTF path resolution relative to cwd vs scene-file dir | v1: paths are relative to the sandbox working directory (next to the exe, where assets are copied), matching how every other game resolves assets. Document this; scene-relative paths can come later. |
| A bad mesh/texture path crashes the loader | Resolve failures log a warning and skip that entity (or use a default); the scene still renders. |
| Legacy `Scene.h` confusion | New types live in `SceneFormat.h`; reviewers/readers see they're distinct. |

## Verification Command

```powershell
cmake --build build-vk --target test_scene_io sandbox --config Debug
ctest --test-dir build-vk -C Debug -R scene_io --output-on-failure
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

Expected: round-trip tests pass; sandbox renders `demo.json` (floor + cubes + a glTF) with a free-fly camera.
