# M22.5 glTF Texture Loading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `iron::GltfLoader` to also return material texture paths (base color, normal map, metallic-roughness). Wire `games/10-gltf-viewer` to load + bind those textures via existing engine helpers so the Damaged Helmet renders with its actual textures applied.

**Architecture:** Add `GltfMaterialPaths` + `GltfModel` data types in `engine/asset/GltfLoader.h`; add `loadGltfModel(path)` that returns mesh + resolved texture file paths (engine module stays pure-data — no `Renderer` dependency); keep existing `loadGltfMesh` as a thin wrapper so tests don't change. Demo loads textures via existing `renderer.loadTexture` (for albedo + normal) and `iron::loadRoughnessAsSpec` + `renderer.createTexture` (for metallic-roughness, mirroring the `games/03-showcase` and `games/07-net-shooter` Polyhaven-roughness pattern).

**Tech Stack:** C++23, tinygltf (header-only, already in M22), Vulkan 1.3, CMake, MSVC.

---

## File Structure

### Modified
- `engine/asset/GltfLoader.h` — add `GltfMaterialPaths` + `GltfModel`; declare `loadGltfModel`
- `engine/asset/GltfLoader.cpp` — implement `loadGltfModel`; refactor `loadGltfMesh` to wrap it
- `tests/test_gltf_loader.cpp` — one new sub-test for material paths
- `games/10-gltf-viewer/main.cpp` — call `loadGltfModel`, bind the three textures
- `docs/engine/asset-pipeline.md` — append "Material textures" section

No new files; no CMake changes.

---

## Task 1: glTF texture loading (loader + demo + tests + docs)

**Files:**
- Modify: `engine/asset/GltfLoader.h`
- Modify: `engine/asset/GltfLoader.cpp`
- Modify: `tests/test_gltf_loader.cpp`
- Modify: `games/10-gltf-viewer/main.cpp`
- Modify: `docs/engine/asset-pipeline.md`

Single-task milestone. After this, the Damaged Helmet renders with proper textures via the existing Blinn-Phong shader.

- [ ] **Step 1: Extend `engine/asset/GltfLoader.h`**

Replace the file's existing contents (or insert appropriately) so it ends up as:

```cpp
#pragma once

#include "scene/Mesh.h"

#include <optional>
#include <string>

namespace iron {

// Absolute paths to a glTF material's textures. Empty string means
// "not present in the file" (or unsupported — e.g., embedded data URIs
// are skipped in v1, leaving the path empty).
struct GltfMaterialPaths {
    std::string albedo;          // pbrMetallicRoughness.baseColorTexture
    std::string normal;          // normalTexture
    std::string metalRoughness;  // pbrMetallicRoughness.metallicRoughnessTexture
                                  // (engine treats G channel as roughness →
                                  // inverted to spec via loadRoughnessAsSpec)
};

struct GltfModel {
    MeshData            mesh;
    GltfMaterialPaths   materialPaths;
};

// Load mesh + material texture paths from a glTF or GLB file.
//
// Same scene-walk and attribute mapping as loadGltfMesh below.
// Additionally reads the first primitive's `material` (if present) and
// resolves the three texture URIs relative to the .gltf file's parent
// directory. Image data is NOT loaded — the caller invokes its own
// texture-load path with the returned absolute paths.
//
// Embedded base64 textures (data: URIs) are NOT supported — those
// paths come back empty. File-URI textures only.
std::optional<GltfModel> loadGltfModel(const std::string& path);

// Backward-compatible: returns just the mesh (drops material paths).
// Implemented as a thin wrapper over loadGltfModel.
std::optional<MeshData> loadGltfMesh(const std::string& path);

}  // namespace iron
```

- [ ] **Step 2: Extend `engine/asset/GltfLoader.cpp`**

Open `engine/asset/GltfLoader.cpp`. **Refactor** the existing `loadGltfMesh` implementation into `loadGltfModel`, then add a small `loadGltfMesh` wrapper.

The high-level shape: rename the body of `loadGltfMesh` to `loadGltfModel`, change the return type, build a `GltfModel`, and add a material-paths-extraction block before the return.

Concrete steps inside the function (after the existing geometry-extraction code that builds `MeshData out`):

1. Add `#include <filesystem>` near the existing standard-library includes (already present per M22). No new external include.
2. After the existing `out.indices = std::move(indices);` line, but before the existing `Log::info(...)` line, add the material-paths extraction:

```cpp
    // M22.5 — material texture paths. Empty if the primitive lacks a
    // material or the material lacks the corresponding texture.
    GltfMaterialPaths matPaths;
    if (prim.material >= 0 &&
        prim.material < static_cast<int>(model.materials.size())) {
        const auto& mat = model.materials[prim.material];
        const std::filesystem::path gltfDir =
            std::filesystem::absolute(path).parent_path();

        auto resolve = [&](int textureIndex) -> std::string {
            if (textureIndex < 0 ||
                textureIndex >= static_cast<int>(model.textures.size())) {
                return {};
            }
            const auto& tex = model.textures[textureIndex];
            if (tex.source < 0 ||
                tex.source >= static_cast<int>(model.images.size())) {
                return {};
            }
            const auto& img = model.images[tex.source];
            // Skip embedded base64 (data URIs) — only file URIs supported.
            if (img.uri.empty() || img.uri.substr(0, 5) == "data:") {
                return {};
            }
            return (gltfDir / img.uri).string();
        };

        matPaths.albedo         = resolve(mat.pbrMetallicRoughness.baseColorTexture.index);
        matPaths.normal         = resolve(mat.normalTexture.index);
        matPaths.metalRoughness = resolve(mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
    }
```

3. Change the function's signature to `std::optional<GltfModel> loadGltfModel(const std::string& path)`.
4. Change the final return to:

```cpp
    GltfModel result;
    result.mesh = std::move(out);
    result.materialPaths = std::move(matPaths);

    Log::info("GltfLoader: loaded %s - %zu verts, %zu indices",
              path.c_str(), result.mesh.vertices.size(), result.mesh.indices.size());
    return result;
```

5. After `loadGltfModel`, add the small wrapper:

```cpp
std::optional<MeshData> loadGltfMesh(const std::string& path) {
    auto model = loadGltfModel(path);
    if (!model) return std::nullopt;
    return std::move(model->mesh);
}
```

(The old free-standing `loadGltfMesh` body is gone — its logic now lives inside `loadGltfModel`.)

- [ ] **Step 3: Add a material-paths sub-test to `tests/test_gltf_loader.cpp`**

Open `tests/test_gltf_loader.cpp`. After the existing BoxTextured.gltf sub-test (which uses `loadGltfMesh`), add a new sub-test:

```cpp
    // --- M22.5: BoxTextured.gltf has a material with a base-color texture ---
    {
        auto model = loadGltfModel(base + "/BoxTextured.gltf");
        CHECK(model.has_value());
        CHECK(!model->materialPaths.albedo.empty());
        // Path should end with the texture file name.
        CHECK(model->materialPaths.albedo.find("CesiumLogoFlat.png") != std::string::npos);
        // BoxTextured has no normal or metallic-roughness texture.
        CHECK(model->materialPaths.normal.empty());
        CHECK(model->materialPaths.metalRoughness.empty());
    }
```

(Place it after the existing BoxTextured `loadGltfMesh` test block, before the Triangle test.)

- [ ] **Step 4: Build + run the test**

```
cmake --build build-vk --config Debug --target ironcore test_gltf_loader
ctest --test-dir build-vk -C Debug -R test_gltf_loader --output-on-failure
```

Expected: PASS. All existing sub-tests + the new material-paths check.

If the material-paths sub-test fails with `albedo is empty`: the BoxTextured.gltf vendored in M22 might use a slightly different texture reference path. Check `tests/assets/gltf/BoxTextured.gltf` (it's JSON — easy to read). Look for the `materials[0].pbrMetallicRoughness.baseColorTexture.index` and trace `textures[that index].source` → `images[that index].uri`. The URI should be `"CesiumLogoFlat.png"` and `resolve()` should produce `<repo>/tests/assets/gltf/CesiumLogoFlat.png`.

If the test fails with `albedo doesn't contain CesiumLogoFlat.png`: the path resolution might be producing back-slashes on Windows. Adjust the assertion to also accept that, or use `std::filesystem::path::filename()` and compare to `"CesiumLogoFlat.png"` directly:

```cpp
CHECK(std::filesystem::path(model->materialPaths.albedo).filename().string()
      == "CesiumLogoFlat.png");
```

- [ ] **Step 5: Update `games/10-gltf-viewer/main.cpp`**

Open the file. Find the existing model-load + mesh-create block (search for `iron::loadGltfMesh`). Replace it with the model + texture-loading version.

The current code is roughly:

```cpp
auto data = iron::loadGltfMesh(modelPath);
if (!data) {
    iron::Log::error("gltf-viewer: failed to load %s", modelPath.c_str());
    return 1;
}
iron::Log::info("gltf-viewer: loaded %zu verts, %zu indices",
                data->vertices.size(), data->indices.size());

const iron::MeshHandle mesh = renderer.createMesh(*data);
```

Replace with:

```cpp
auto model = iron::loadGltfModel(modelPath);
if (!model) {
    iron::Log::error("gltf-viewer: failed to load %s", modelPath.c_str());
    return 1;
}
iron::Log::info("gltf-viewer: loaded %zu verts, %zu indices",
                model->mesh.vertices.size(), model->mesh.indices.size());

const iron::MeshHandle mesh = renderer.createMesh(model->mesh);

// M22.5 — load material textures via existing engine helpers.
const iron::TextureHandle albedo = model->materialPaths.albedo.empty()
    ? renderer.whiteTexture()
    : renderer.loadTexture(model->materialPaths.albedo);
const iron::TextureHandle normalMap = model->materialPaths.normal.empty()
    ? renderer.flatNormalTexture()
    : renderer.loadTexture(model->materialPaths.normal);

iron::TextureHandle spec = renderer.noSpecularTexture();
if (!model->materialPaths.metalRoughness.empty()) {
    int w = 0, h = 0;
    auto specBytes = iron::loadRoughnessAsSpec(
        model->materialPaths.metalRoughness, w, h);
    if (!specBytes.empty()) {
        spec = renderer.createTexture(w, h, specBytes.data());
    }
}
```

Then find the draw-call construction in the render lambda. Update the material binding from defaults to the loaded textures:

```cpp
// Old:
// call.material.texture     = renderer.whiteTexture();
// call.material.normalMap   = renderer.flatNormalTexture();
// call.material.specularMap = renderer.noSpecularTexture();

// New:
call.material.texture     = albedo;
call.material.normalMap   = normalMap;
call.material.specularMap = spec;
```

Make sure `albedo`, `normalMap`, and `spec` are captured by the render lambda (they're declared in `main()`'s scope — the lambda already uses `[&]` capture so they're available).

Add `#include "render/TextureLoader.h"` near the top of the file (it's where `iron::loadRoughnessAsSpec` lives — confirm by reading the existing `games/03-showcase/main.cpp` or `games/07-net-shooter/main.cpp` for the include line).

- [ ] **Step 6: Build the demo**

```
cmake --build build-vk --config Debug --target gltf-viewer
```

Expected: clean compile.

If `iron::loadRoughnessAsSpec` is undefined: confirm the include is correct (`render/TextureLoader.h`).

If the linker complains about missing `loadGltfModel`: you forgot Step 2's signature change or the new function isn't actually defined. Double-check the .cpp.

- [ ] **Step 7: Visual smoke test**

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe
```

Expected:
- Damaged Helmet now shows **its actual textures** — rust spots, scratched panels, brushed metal surface detail visible.
- Normal map gives the surface noticeable depth (lighting plays across the rivets and dents).
- Spec map (inverted metallic-roughness) gives the metal parts some shine; rusted parts stay matte.
- Free-fly camera still works (WASD + mouse).
- HUD vert/tri counts unchanged.

If the helmet looks flat-textured (correct colors but no surface depth): the normal map isn't being bound. Check that `model->materialPaths.normal` isn't empty (the Damaged Helmet definitely has a normal map), and that the load path resolves correctly.

If the helmet looks blown-out / saturated: the spec map inversion might be too aggressive. Acceptable v1; PBR proper will fix lighting.

- [ ] **Step 8: Update `docs/engine/asset-pipeline.md`**

Open `docs/engine/asset-pipeline.md`. Find the "What's NOT loaded in M22" section. Add a new section AFTER it (and before "Visual validator"):

```markdown
## M22.5 — Material textures

The loader's primary API is now `loadGltfModel(path)`, returning both
the `MeshData` and a `GltfMaterialPaths` struct with absolute file
paths to the first primitive's material textures (base color, normal,
metallic-roughness). `loadGltfMesh` becomes a thin wrapper for callers
that only need geometry.

```cpp
struct GltfMaterialPaths {
    std::string albedo;
    std::string normal;
    std::string metalRoughness;
};

struct GltfModel {
    MeshData            mesh;
    GltfMaterialPaths   materialPaths;
};

std::optional<GltfModel> loadGltfModel(const std::string& path);
```

The loader returns paths only — image data is NOT loaded. Game code
calls `renderer.loadTexture(path)` for color + normal, and
`iron::loadRoughnessAsSpec(path, w, h)` + `renderer.createTexture(...)`
for the metallic-roughness map (the engine's "spec" slot expects
bright = shiny, so the helper inverts the G channel from glTF's
roughness convention). Empty paths fall back to engine defaults
(`whiteTexture` / `flatNormalTexture` / `noSpecularTexture`).

Embedded base64 textures (`data:` URIs) are NOT supported in v1 —
those paths come back empty. File-URI textures only.

The 10-gltf-viewer demo now shows the Damaged Helmet with its full
PBR textures applied (under the existing Blinn-Phong shader — proper
PBR is a future track).
```

Also update the "What's NOT loaded in M22" section: remove the
"Textures / materials" bullet (it's loaded now) and replace with:

```markdown
- **PBR shading.** Textures are bound through the existing Blinn-Phong
  lit shader. Metallic-roughness conversion to "spec map" is
  approximate (G channel only, metallic discarded). Proper PBR
  (metallic-roughness BRDF + image-based lighting + tone-mapping) is a
  future track.
```

- [ ] **Step 9: Commit + push + open PR**

```
git add engine/asset/GltfLoader.h engine/asset/GltfLoader.cpp \
        tests/test_gltf_loader.cpp \
        games/10-gltf-viewer/main.cpp \
        docs/engine/asset-pipeline.md
git commit -m "M22.5: glTF material texture loading (Damaged Helmet now textured)"
git push -u origin feat/m22-5-gltf-texture-loading
gh pr create --title "M22.5: glTF material texture loading" --body "$(cat <<'EOF'
## Summary
- New `iron::loadGltfModel(path)` returns mesh + `GltfMaterialPaths` (absolute file paths to baseColor / normal / metallic-roughness textures)
- `iron::loadGltfMesh` becomes a thin wrapper (tests + simple callers unchanged)
- Texture URI resolution: prepend the .gltf file's parent dir; skip embedded `data:` URIs (file URIs only in v1)
- `games/10-gltf-viewer` now loads + binds the three textures via existing engine helpers — Damaged Helmet renders with full texture detail
- One new test sub-check on BoxTextured.gltf material paths
- `docs/engine/asset-pipeline.md` extended with the M22.5 section

No shader changes; no new dependencies. Pure asset-pipeline + game-side wiring.

## Test plan
- [ ] CI green (Windows MSVC) — `test_gltf_loader` includes the new material-paths sub-check
- [ ] `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe` shows the Damaged Helmet with rust, panel detail, normal-map surface (real textures, not flat-shaded white)

## Known v1 limitations
- File-URI textures only (no embedded base64)
- Metallic-roughness → spec uses G channel only; metallic discarded (proper PBR is its own future track)
- No emissive / AO map slots (engine `Material` lacks those — needs shader changes)
- Single primitive's material only (matches M22's single-primitive scope)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- `GltfMaterialPaths` + `GltfModel` types → Step 1
- `loadGltfModel(path)` → Step 2
- `loadGltfMesh` wrapper → Step 2
- Texture URI resolution against gltf parent dir → Step 2 (`resolve` lambda)
- Skip `data:` URIs → Step 2 (`if (img.uri.substr(0, 5) == "data:") return {};`)
- BoxTextured material-paths test → Step 3
- Demo wiring (albedo + normal via `loadTexture`, metallic-roughness via `loadRoughnessAsSpec`) → Step 5
- Docs update → Step 8

**Placeholder scan:** clean — every code step has complete code.

**Type consistency:**
- `GltfMaterialPaths` field names (`albedo`, `normal`, `metalRoughness`) consistent across header, impl, test, demo, docs
- `loadGltfModel` return type `std::optional<GltfModel>` consistent
- `loadGltfMesh` wrapper signature unchanged (tests + other callers don't break)
- `iron::loadRoughnessAsSpec(path, &w, &h)` signature matches what the demo uses (confirmed from `engine/render/TextureLoader.h`)
