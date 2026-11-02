#pragma once

#include "asset/Animation.h"
#include "math/Aabb.h"
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
                                  // (glTF layout: .g = roughness, .b = metallic;
                                  // consumed directly as the PBR MR map since M45b)
    std::string occlusion;       // occlusionTexture (.r = AO; linear)
    std::string emissive;        // emissiveTexture (sRGB)
};

struct GltfModel {
    MeshData                       mesh;
    GltfMaterialPaths              materialPaths;
    std::optional<SkinnedMeshData> skinnedMesh;  // populated if glTF has a skin
    std::vector<AnimationClip>     animations;   // empty if file has no animations

    // PBR scalar factors (glTF defaults: metallic/roughness 1, baseColor {1,1,1},
    // emissive {0,0,0}). Multiply the corresponding maps.
    float metallicFactor  = 1.0f;
    float roughnessFactor = 1.0f;
    Vec3  baseColorFactor{1.0f, 1.0f, 1.0f};
    Vec3  emissiveFactor{0.0f, 0.0f, 0.0f};
    float normalScale     = 1.0f;  // glTF normalTexture.scale (0=flat, >1=exaggerated)

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

// A glTF material: its textures' paths and its PBR factors.
struct GltfSurface {
    GltfMaterialPaths materialPaths;
    float metallicFactor  = 1.0f;
    float roughnessFactor = 1.0f;
    Vec3  baseColorFactor{1.0f, 1.0f, 1.0f};
    Vec3  emissiveFactor{0.0f, 0.0f, 0.0f};
    float normalScale     = 1.0f;
};

// One part of a static model that shares a material: a material slot, in Unreal's
// terms. Vertices are in the model's space, with every node's transform applied.
struct GltfSection : GltfSurface {
    std::string node;  // The node it came from, when split by node.
    MeshData    mesh;
};

// Every mesh in a glTF scene, not just the first: the whole node tree is walked,
// node transforms are baked into the vertices, and primitives are merged by
// material. Tangents missing from the file are computed from the UVs. Skins and
// animation are ignored; use loadGltfModel for a skinned mesh.
struct GltfStaticModel {
    std::vector<GltfSection> sections;
    Aabb bounds{};
};

struct GltfStaticOptions {
    // Keep each node's primitives apart, so that parts can move on their own (a
    // rifle's bolt, a magazine). Otherwise everything with one material merges.
    bool splitByNode = false;
};

std::optional<GltfStaticModel> loadGltfStaticModel(const std::string& path,
                                                   const GltfStaticOptions& options = {});

// One material slot of a skinned model. Its mesh's own skeleton is left empty: every
// section shares the model's.
struct GltfSkinnedSection : GltfSurface {
    SkinnedMeshData mesh;
};

// A skeletal mesh, in Unreal's terms: every skinned mesh in the file bound to its
// skin, split by material, with the skeleton and every animation clip. A first-
// person rig of arms and rifle, say, with separate materials for each.
struct GltfSkinnedModel {
    Skeleton                        skeleton;
    std::vector<GltfSkinnedSection> sections;
    std::vector<AnimationClip>      animations;

    const AnimationClip* findClip(std::string_view name) const;
    // The bone's index, or -1.
    int findBone(std::string_view name) const;
};

// Reads the first skin's meshes. Meshes bound to another skin are skipped with a
// warning. Fails when nothing in the file is skinned.
std::optional<GltfSkinnedModel> loadGltfSkinnedModel(const std::string& path);

}  // namespace iron
