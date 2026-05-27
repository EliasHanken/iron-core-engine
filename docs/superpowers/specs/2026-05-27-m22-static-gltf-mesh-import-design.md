# M22 — Static glTF Mesh Import (Design Spec)

**Date:** 2026-05-27
**Milestone:** M22 (first of the skeletal-animation + glTF asset pipeline track)
**Status:** Design — awaiting implementation plan

## Goal

Load a single static mesh from a glTF or GLB file into the engine's existing `iron::MeshData` format. Ship a visual validator (`games/10-gltf-viewer`) that loads Khronos's "Damaged Helmet" CC0 sample and renders it under the existing Vulkan lit shader. **Geometry only** — textures, skeleton, animations are out of scope; they land in later milestones of this track.

After M22, the engine has a working glTF foundation: a thin loader API, a vendored set of test assets, and a tested mapping from glTF attributes to the engine's vertex layout. M23 (skinning) builds on this loader by extending it for skeleton + bone-weight data; M24 adds animation curves; M25 wires skinned characters into net-shooter.

## Direction context

The physics overhaul track (M18-M21) is complete. Net-shooter has working movement, projectiles, ragdoll death — but every visible character is still a colored cube. The biggest visual gap to a TF2/Overwatch-class look is **skinned meshes with animations**. That's a 4-milestone track (M22-M25); M22 is the foundation.

The engine direction shift (2026-05-27, [[iron-core-engine-learning-goal]]): use mature libraries when they're a better fit than hand-rolling. **tinygltf** is the de facto standard glTF C++ library — MIT, header-only-ish, available in vcpkg. Hand-rolling a glTF parser would be a multi-week detour for no payoff. Same call as Jolt for physics.

## Non-Goals

- **Textures from glTF**, including the base-color map. The Damaged Helmet has prominent PBR textures (albedo + normal + metallic-roughness) baked into the file. M22 ignores all of them — game code passes engine defaults (`whiteTexture` / `flatNormalTexture` / `noSpecularTexture`). The helmet will look flat-shaded but recognizable. Texture loading is a small follow-up or part of M23.
- **Skeleton, joints, skinning, animations** — track milestones M23/M24/M25.
- **Multi-primitive, multi-mesh, scene-graph traversal.** The loader handles "first primitive of first mesh in scene[0].nodes[0]" only. Multi-mesh / scene hierarchy may be needed later for skinned characters but is not part of M22's loader.
- **MikkTSpace tangent synthesis.** glTF spec says exporters SHOULD provide tangents for normal-mapped meshes; if a file is missing tangents, M22 falls back to `(1, 0, 0)` rather than synthesizing them. (Damaged Helmet ships tangents, so this is purely a fallback.)
- **glTF 2.0 extensions** (`KHR_draco_compression`, `KHR_texture_basisu`, `KHR_lights_punctual`, etc.) — none of the v1 demo assets use them.
- **PBR shading.** The engine renders glTF meshes through the existing Blinn-Phong lit shader. Material upgrade is its own track.
- **Runtime mesh editing** (LOD, merging, simplification). Out of scope.
- **Net-shooter integration.** That's M25.

## Architecture

### 1. `engine/asset/GltfLoader.h/.cpp`

New engine subsystem. Public surface is one function:

```cpp
#pragma once

#include "scene/Mesh.h"

#include <optional>
#include <string>

namespace iron {

// Loads the first primitive of the first mesh of the first node of the
// default scene. Returns std::nullopt on parse failure, missing required
// attributes (POSITION, NORMAL, or indices), or if the file's accessor
// types don't map to engine's Vertex layout.
//
// Path can be .gltf (JSON + adjacent .bin) or .glb (single binary file).
// tinygltf auto-detects from the extension.
//
// Attribute mapping (glTF -> engine Vertex):
//   POSITION   (vec3) -> Vertex::position    [required]
//   NORMAL     (vec3) -> Vertex::normal      [required]
//   TEXCOORD_0 (vec2) -> Vertex::uv          [optional; defaults to (0,0)]
//   TANGENT    (vec4) -> Vertex::tangent     [optional; xyz only, w sign dropped; defaults to (1,0,0)]
//   indices    (u16/u32) -> u32 indices      [required; u16 promoted to u32]
//
// No texture / material / skin / animation data is loaded.
std::optional<MeshData> loadGltfMesh(const std::string& path);

}  // namespace iron
```

### 2. `engine/asset/GltfLoader.cpp` implementation outline

- `#include` tinygltf in the .cpp only (not in the public header).
- `tinygltf::TinyGLTF` parser. Call `LoadASCIIFromFile` for `.gltf`, `LoadBinaryFromFile` for `.glb`. Dispatch on extension.
- Check returned `tinygltf::Model`:
  - `model.scenes` non-empty, `model.scenes[model.defaultScene].nodes` non-empty.
  - Walk into the first node, find its `mesh` index.
  - First primitive of that mesh.
- For each required attribute (`POSITION`, `NORMAL`), look up the accessor + buffer view + raw bytes. Validate component type + count. Read as float triples.
- For each optional attribute (`TEXCOORD_0`, `TANGENT`), do the same if present; else use default.
- Indices: read the accessor, promote u16 → u32 if needed.
- Build a `std::vector<iron::Vertex>` of length `accessor.count` for positions.
- Build a `std::vector<uint32_t>` of indices.
- Return `MeshData{vertices, indices}`.

Helper functions kept private to the .cpp:
- `readVec3Accessor(model, accessorIdx) -> std::vector<Vec3>`
- `readVec2Accessor(model, accessorIdx) -> std::vector<Vec2>`
- `readIndicesAccessor(model, accessorIdx) -> std::vector<uint32_t>` (handles u16/u32)

All tinygltf types stay in the .cpp. No game-side or engine-public exposure.

### 3. Test glTFs (Khronos sample suite, CC0)

Vendor a minimal test set under `tests/assets/gltf/`. Pick the smallest possible files that exercise each path:

| File | Size | What it tests |
| ---- | ---- | ------------- |
| `Box.gltf` + `Box0.bin` | ~3 KB | Minimal geometry, positions + normals + indices, no UVs |
| `BoxTextured.gltf` + `Box0.bin` + textures | ~5 KB | Geometry + UVs (textures vendored but unused by loader) |
| `Triangle.gltf` (inline buffer) | ~1 KB | Single triangle, embedded buffer (data URI) |

All are pulled from `github.com/KhronosGroup/glTF-Sample-Models/2.0/` — CC0 licensed.

### 4. Vendored demo asset

`games/10-gltf-viewer/assets/damaged-helmet/DamagedHelmet.gltf` (+ `DamagedHelmet.bin` + `Default_albedo.jpg` etc.). Total ~200 KB. Checked into the repo (no LFS at this size). From Khronos's CC0 sample set.

### 5. `games/10-gltf-viewer` demo

Vulkan-only (matches the post-PR-#35 default backend). Minimal structure mirroring `games/08-particle-storm` and `games/09-physics-playground`:

```cpp
// games/10-gltf-viewer/main.cpp (sketch)
int main() {
    Application app({title="Iron Core - glTF Viewer", 1280, 720});
    auto renderer = createRenderer(app.window());

    auto data = loadGltfMesh(executableDir() + "/assets/damaged-helmet/DamagedHelmet.gltf");
    if (!data) { Log::error("failed to load model"); return 1; }
    MeshHandle mesh = renderer->createMesh(*data);
    ShaderHandle shader = renderer->createShader(/* lit shader, GLSL 450 */);

    FreeFlyCamera cam{position={0, 0, 3}, yaw=..., pitch=...};

    Hud hud;
    HudId statsId = hud.addText(...);  // "Verts: N  Tris: N/3"

    Application::setUpdate([&](FrameTime t) {
        // WASD + mouse fly
        cam.update(...);
    });
    Application::setRender([&]() {
        const Mat4 view = cam.viewMatrix();
        const Mat4 proj = perspective(...);
        renderer->beginFrame({0.55, 0.45, 0.35}, dimSun, {}, Fog{}, view, proj);
        DrawCall call;
        call.mesh = mesh;
        call.shader = shader;
        call.model = identity();
        call.material.texture     = renderer->whiteTexture();
        call.material.normalMap   = renderer->flatNormalTexture();
        call.material.specularMap = renderer->noSpecularTexture();
        renderer->submit(call);
        renderer->drawHud(hud.build(font, renderer->whiteTexture()), 1280, 720);
        renderer->endFrame();
        app.window().swapBuffers();
    });
    app.run();
}
```

Camera initial position chosen so the helmet is visible (Damaged Helmet is ~2m wide, centered at origin; spawn camera at `(0, 0, 3)` looking at origin).

HUD shows:
- Vertex count
- Triangle count
- Model bounds (min/max y) — useful for confirming geometry loaded sanely
- Key hints (`WASD: move  mouse: look  ESC: quit`)

### 6. Build integration

- `vcpkg.json` gains `"tinygltf"` alongside the existing deps.
- Top-level `CMakeLists.txt` gains `find_package(tinygltf CONFIG REQUIRED)` after the existing `find_package(unofficial-joltphysics ...)`.
- `engine/CMakeLists.txt` registers `asset/GltfLoader.cpp` in the `ironcore` sources; links `tinygltf::tinygltf` PRIVATE (so game code linking `ironcore` doesn't transitively pull tinygltf headers).
- `games/10-gltf-viewer/CMakeLists.txt` gates on Vulkan (`if (IRON_RENDER_BACKEND STREQUAL "vulkan")`).
- `tests/CMakeLists.txt` adds `iron_add_test(test_gltf_loader test_gltf_loader.cpp)`; pass repo root via `IRON_REPO_ROOT` define so the test finds `tests/assets/gltf/` (same pattern as `test_texture_loader`).

## Tasks

Three subagent-friendly chunks:

1. **vcpkg + `iron::GltfLoader` + unit tests** — `tinygltf` in manifest, top-level find_package, engine wrapper, link PRIVATE. Vendor `tests/assets/gltf/Box.gltf`, `BoxTextured.gltf`, `Triangle.gltf`. Tests: load each, assert vertex/index counts + a sample position, plus invalid-path returning `nullopt`. Standalone — no game uses it yet.

2. **`games/10-gltf-viewer` demo** — vendor Damaged Helmet assets, write main.cpp + CMakeLists.txt, Vulkan-only gate. Free-fly camera, single mesh render, HUD with vert/tri counts. Visual validator.

3. **Docs** — new `docs/engine/asset-pipeline.md` covering the glTF loader API, attribute mapping, what's deferred (textures, skeleton, anims), and the M22-M25 track plan. (This is the first track in a fresh docs section — sets up scaffolding for M23+.)

## Tests

`tests/test_gltf_loader.cpp`:

```cpp
#include "asset/GltfLoader.h"
#include "test_framework.h"

#include <cstdio>
#include <string>

int main() {
    using namespace iron;

    // --- Load Box.gltf: 24 vertices, 36 indices (a unit cube) ---
    {
        auto data = loadGltfMesh(std::string(IRON_REPO_ROOT) + "/tests/assets/gltf/Box.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 24);
        CHECK(data->indices.size()  == 36);
        // First vertex's position is well-defined for Box.gltf (one of the corners).
        // Sanity-check: all positions are within the unit-cube AABB.
        for (const auto& v : data->vertices) {
            CHECK(std::abs(v.position.x) <= 0.51f);
            CHECK(std::abs(v.position.y) <= 0.51f);
            CHECK(std::abs(v.position.z) <= 0.51f);
        }
    }

    // --- Load BoxTextured.gltf: same geometry, UVs present ---
    {
        auto data = loadGltfMesh(
            std::string(IRON_REPO_ROOT) + "/tests/assets/gltf/BoxTextured.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 24);
        // At least one UV is non-zero (defaults are (0,0); real UVs span [0,1]).
        bool sawNonZeroUv = false;
        for (const auto& v : data->vertices) {
            if (v.uv.x != 0.0f || v.uv.y != 0.0f) { sawNonZeroUv = true; break; }
        }
        CHECK(sawNonZeroUv);
    }

    // --- Load Triangle.gltf: 3 verts, 3 indices ---
    {
        auto data = loadGltfMesh(
            std::string(IRON_REPO_ROOT) + "/tests/assets/gltf/Triangle.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 3);
        CHECK(data->indices.size()  == 3);
    }

    // --- Invalid path returns nullopt ---
    {
        auto data = loadGltfMesh("/this/path/does/not/exist.gltf");
        CHECK(!data.has_value());
    }

    return iron_test_result();
}
```

No engine-level visual test; the demo IS the visual validator.

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| `tinygltf` vcpkg port quality | Port has been stable for years (used by many engines via vcpkg). CI catches first if broken. |
| Damaged Helmet looks flat without textures | Documented v1 limitation. Geometry-only render still proves the loader works; textures land in a follow-up. |
| glTF attribute layout assumptions (interleaved vs non-interleaved buffer views) | tinygltf abstracts this — the loader works against the parsed `Model` not raw bytes. We pay a small CPU cost for each accessor read but correctness is glued. |
| u16 → u32 index promotion inflates memory | ~2× for indices only. Damaged Helmet ~46K indices = ~180 KB after promotion. Trivial. |
| `tinygltf` transitively includes a heavy `json.hpp` | Confined to one .cpp via PRIVATE link. No header bleed. |
| TANGENT default of `(1, 0, 0)` is wrong for models lacking tangents | Acceptable v1; visual artifact only. Damaged Helmet ships tangents. MikkTSpace synthesis is a future polish. |
| Vendored test glTFs come from a CC0 source but with multiple files (.gltf + .bin + textures) | All CC0, copy-paste OK. Khronos's sample-models repo is the upstream truth. |

## Verification

- **CI green** on Windows MSVC (new tests pass; tinygltf compiles in CI's vcpkg pipeline).
- **Solo run**: `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe` opens a window with the Damaged Helmet visible. Free-fly to orbit it. HUD shows verts/tris.
- **No regression** in other games or engine tests.

## Follow-ups (NOT in M22)

- **M23** — Skeleton + skinning shader. Extend loader to read `skin` data; new `iron::Skeleton` + `iron::SkinnedMesh` types; Vulkan skinning vertex shader.
- **M24** — Animation curves + playback.
- **M25** — Wire skinned characters into net-shooter.
- Optional follow-up between M22 and M23: load textures from glTF (base-color → diffuse, normal → normalMap, metallic-roughness → spec). Small task, ~100 LOC.
- MikkTSpace tangent synthesis for models without baked tangents.
- Multi-mesh / scene-graph traversal (probably needed for skinned characters that have multiple mesh primitives, e.g., head + body).
- `KHR_draco_compression` extension support (if we ever want to ship compressed assets).
