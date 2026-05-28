#pragma once

#include "asset/Animation.h"
#include "scene/Mesh.h"
#include "scene/SkinnedMesh.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    MeshData                       mesh;
    GltfMaterialPaths              materialPaths;
    std::optional<SkinnedMeshData> skinnedMesh;  // populated if glTF has a skin
    std::vector<AnimationClip>     animations;   // empty if file has no animations

    // Returns a pointer to the first clip whose name matches `name`,
    // or nullptr if no match. Linear scan; clip counts are tiny in v1.
    const AnimationClip* findClip(std::string_view name) const;
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
