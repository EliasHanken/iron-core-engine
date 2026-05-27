# M22.5 — glTF Texture Loading (Design Spec)

**Date:** 2026-05-27
**Milestone:** M22.5 (small follow-up to M22, before M23)
**Status:** Design — awaiting implementation plan

## Goal

Wire glTF material textures (base color, normal map, metallic-roughness) through the existing engine `Material` slots. After M22.5 lands, the Damaged Helmet renders with its actual textures via the existing Blinn-Phong lit shader instead of flat-white. No shader changes; pure asset-pipeline + game-side wiring.

## Direction context

M22 shipped geometry-only glTF import — the helmet rendered as a flat-white mesh because the loader skipped images. This follow-up closes the obvious visual gap before M23 (skinning) takes over. Smaller than a full milestone — single task — but per brainstorming process gets a short spec.

PBR shading proper is its own future track (M-PBR-1 to M-PBR-3 from earlier discussion). M22.5 stays with Blinn-Phong; the metallic-roughness texture maps into the engine's "spec map" slot via the existing `loadRoughnessAsSpec` helper (matches how `games/03-showcase` and `games/07-net-shooter` use Polyhaven roughness maps).

## Non-Goals

- **PBR shader.** Future track. M22.5 reuses the existing Blinn-Phong lit shader unchanged.
- **Metallic channel.** glTF stores metallic in B of the metallic-roughness texture. M22.5 reads only G (roughness) and inverts to spec; the metallic info is discarded. PBR proper will read both.
- **Embedded base64 textures.** glTF supports inlining images as `data:` URIs in the JSON. M22.5 supports only file-URI textures. Damaged Helmet uses file URIs; the test set has no embedded images.
- **Emissive / AO / occlusion-roughness-metallic packed textures.** Damaged Helmet has separate AO and emissive — both ignored in M22.5. The engine's `Material` doesn't have AO/emissive-map slots; those would require shader changes.
- **Multi-material meshes.** glTF allows different primitives in one mesh to use different materials. M22.5 reads only the first primitive's material (matches M22's "first primitive only" scope).

## Architecture

### Loader API extension

`engine/asset/GltfLoader.h` gains two new types and a function:

```cpp
struct GltfMaterialPaths {
    // Absolute paths (resolved against the .gltf file's parent dir).
    // Empty string means "not present in the file".
    std::string albedo;          // base color texture
    std::string normal;          // normal map
    std::string metalRoughness;  // metallic-roughness (we use G channel as roughness)
};

struct GltfModel {
    MeshData            mesh;
    GltfMaterialPaths   materialPaths;
};

// Load mesh + material texture paths. Same scene-walk and attribute
// mapping as loadGltfMesh; additionally reads the first primitive's
// material and resolves texture URIs to absolute filesystem paths.
//
// Image data is NOT loaded — the renderer loads each texture via its
// own loadTexture / createTexture path (lets the caller choose
// linear vs nearest filtering, sRGB handling, etc.).
//
// Embedded base64 textures are NOT supported in v1 — the path will be
// empty if the URI starts with "data:".
std::optional<GltfModel> loadGltfModel(const std::string& path);
```

The existing `loadGltfMesh(path)` becomes a thin wrapper:

```cpp
std::optional<MeshData> loadGltfMesh(const std::string& path) {
    auto model = loadGltfModel(path);
    if (!model) return std::nullopt;
    return std::move(model->mesh);
}
```

This preserves the existing tests (they use `loadGltfMesh`) without changes.

### Implementation outline

In `GltfLoader.cpp`'s new `loadGltfModel` function:
1. Do the existing scene/mesh/primitive walk from M22.
2. Build the `MeshData` as before.
3. If `prim.material >= 0` and within `model.materials.size()`:
   - Look up the `tinygltf::Material`.
   - For `pbrMetallicRoughness.baseColorTexture.index`: get texture → image → URI. Resolve against the .gltf file's parent dir. Store in `materialPaths.albedo`. Skip if URI starts with `"data:"`.
   - Same for `normalTexture.index` → `materialPaths.normal`.
   - Same for `pbrMetallicRoughness.metallicRoughnessTexture.index` → `materialPaths.metalRoughness`.

The `std::filesystem::path` parent-dir resolution:

```cpp
const std::filesystem::path gltfDir =
    std::filesystem::absolute(path).parent_path();
auto resolve = [&](const std::string& uri) -> std::string {
    if (uri.empty() || uri.substr(0, 5) == "data:") return {};
    return (gltfDir / uri).string();
};
```

### Demo wiring

`games/10-gltf-viewer/main.cpp`:
- Replace `loadGltfMesh` with `loadGltfModel`.
- For each material path, load via the appropriate engine helper:
  - albedo → `renderer.loadTexture(path)` (sRGB, linear filtering)
  - normal → `renderer.loadTexture(path)` (same path; engine treats all textures the same in M22 era)
  - metalRoughness → `iron::loadRoughnessAsSpec(path, w, h)` then `renderer.createTexture(w, h, specBytes.data())` — same pattern as net-shooter/showcase

Each defaults to the engine's `whiteTexture()` / `flatNormalTexture()` / `noSpecularTexture()` if the path is empty.

### Test additions

Extend `tests/test_gltf_loader.cpp` with one more sub-test on `BoxTextured.gltf`:

```cpp
// --- BoxTextured.gltf has a material with a base-color texture ---
{
    auto model = loadGltfModel(base + "/BoxTextured.gltf");
    CHECK(model.has_value());
    CHECK(!model->materialPaths.albedo.empty());
    // Path should end with the texture file name.
    CHECK(model->materialPaths.albedo.find("CesiumLogoFlat.png") != std::string::npos);
    // Normal and metalRoughness not present in this asset.
    CHECK(model->materialPaths.normal.empty());
    CHECK(model->materialPaths.metalRoughness.empty());
}
```

No new test assets needed — BoxTextured already exercises the texture-path resolution.

## Tasks

Single task. Three steps within it:

1. Extend `GltfLoader.h/.cpp` with `GltfMaterialPaths`, `GltfModel`, `loadGltfModel`. Refactor `loadGltfMesh` to wrap it. Add the BoxTextured material-path test.
2. Update `games/10-gltf-viewer/main.cpp` to call `loadGltfModel` and bind the three textures via existing engine helpers.
3. Append a "Material textures" section to `docs/engine/asset-pipeline.md`.

## Verification

- **CI green** — the new test sub-check passes; existing 38 tests still pass.
- **Visual**: `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe` shows the Damaged Helmet with **actual textures applied** — recognizable as a sci-fi battle-damaged helmet (rust spots, panel lines, normal-map surface detail).

## Follow-ups (NOT in M22.5)

- Embedded base64 texture support.
- Emissive / AO texture support — needs shader changes.
- Multi-primitive material handling.
- PBR shading proper (separate M-PBR track).
- Metallic channel — only meaningful under PBR.
