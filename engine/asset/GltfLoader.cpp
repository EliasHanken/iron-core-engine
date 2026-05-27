// GltfLoader.cpp -- tinygltf-backed loader for a single static primitive.
// All tinygltf types live in this translation unit; the public header
// only exposes engine types (MeshData, std::optional, std::string).

#include "asset/GltfLoader.h"
#include "core/Log.h"

// tinygltf needs these defines in exactly one TU before its header.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include <cstdint>
#include <cstring>
#include <filesystem>

namespace iron {

namespace {

std::vector<Vec3> readVec3Accessor(const tinygltf::Model& model, int accessorIdx) {
    std::vector<Vec3> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
    if (acc.type != TINYGLTF_TYPE_VEC3) return out;
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base =
        buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * byteStride);
        out.push_back(Vec3{f[0], f[1], f[2]});
    }
    return out;
}

std::vector<Vec2> readVec2Accessor(const tinygltf::Model& model, int accessorIdx) {
    std::vector<Vec2> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
    if (acc.type != TINYGLTF_TYPE_VEC2) return out;
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base =
        buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * byteStride);
        out.push_back(Vec2{f[0], f[1]});
    }
    return out;
}

std::vector<Vec3> readVec4AsVec3Accessor(const tinygltf::Model& model, int accessorIdx) {
    std::vector<Vec3> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
    if (acc.type != TINYGLTF_TYPE_VEC4) return out;
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base =
        buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * byteStride);
        out.push_back(Vec3{f[0], f[1], f[2]});
    }
    return out;
}

std::vector<std::uint32_t> readIndicesAccessor(const tinygltf::Model& model, int accessorIdx) {
    std::vector<std::uint32_t> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.type != TINYGLTF_TYPE_SCALAR) return out;
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base =
        buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * byteStride;
        std::uint32_t idx = 0;
        switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                std::uint16_t v;
                std::memcpy(&v, p, sizeof(v));
                idx = v;
            } break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                std::memcpy(&idx, p, sizeof(idx));
            } break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                idx = *p;
            } break;
            default:
                return {};
        }
        out.push_back(idx);
    }
    return out;
}

}  // namespace

std::optional<GltfModel> loadGltfModel(const std::string& path) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // Suppress tinygltf's image loading callbacks since we disabled stb_image.
    loader.SetImageLoader(
        [](tinygltf::Image*, const int, std::string*, std::string*, int, int,
           const unsigned char*, int, void*) { return true; },
        nullptr);

    const std::string ext = std::filesystem::path(path).extension().string();
    bool ok = false;
    if (ext == ".glb" || ext == ".GLB") {
        ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    } else {
        ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }

    if (!warn.empty()) Log::warn("GltfLoader: %s", warn.c_str());
    if (!ok) {
        Log::error("GltfLoader: parse failed: %s", err.c_str());
        return std::nullopt;
    }

    if (model.scenes.empty()) {
        Log::error("GltfLoader: no scenes in %s", path.c_str());
        return std::nullopt;
    }
    const int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (sceneIdx < 0 || sceneIdx >= static_cast<int>(model.scenes.size())) {
        Log::error("GltfLoader: invalid defaultScene index");
        return std::nullopt;
    }
    const auto& scene = model.scenes[sceneIdx];
    if (scene.nodes.empty()) {
        Log::error("GltfLoader: scene has no nodes");
        return std::nullopt;
    }

    // Walk scene -> node (and one level of children) to find the first mesh.
    int meshIdx = -1;
    for (const int nodeIdx : scene.nodes) {
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) continue;
        const auto& node = model.nodes[nodeIdx];
        if (node.mesh >= 0) { meshIdx = node.mesh; break; }
        for (const int childIdx : node.children) {
            if (childIdx < 0 || childIdx >= static_cast<int>(model.nodes.size())) continue;
            const auto& child = model.nodes[childIdx];
            if (child.mesh >= 0) { meshIdx = child.mesh; break; }
        }
        if (meshIdx >= 0) break;
    }
    if (meshIdx < 0 || meshIdx >= static_cast<int>(model.meshes.size())) {
        Log::error("GltfLoader: no mesh found in scene");
        return std::nullopt;
    }
    const auto& mesh = model.meshes[meshIdx];
    if (mesh.primitives.empty()) {
        Log::error("GltfLoader: mesh has no primitives");
        return std::nullopt;
    }
    const auto& prim = mesh.primitives[0];

    auto posIt = prim.attributes.find("POSITION");
    auto nrmIt = prim.attributes.find("NORMAL");
    if (posIt == prim.attributes.end() || nrmIt == prim.attributes.end() ||
        prim.indices < 0) {
        Log::error("GltfLoader: primitive missing POSITION/NORMAL/indices");
        return std::nullopt;
    }

    const auto positions = readVec3Accessor(model, posIt->second);
    const auto normals   = readVec3Accessor(model, nrmIt->second);
    if (positions.empty() || normals.empty() || positions.size() != normals.size()) {
        Log::error("GltfLoader: position/normal accessor read failed or mismatch");
        return std::nullopt;
    }

    const std::size_t n = positions.size();

    std::vector<Vec2> uvs;
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
        uvs = readVec2Accessor(model, uvIt->second);
        if (uvs.size() != n) uvs.clear();
    }

    std::vector<Vec3> tangents;
    auto tanIt = prim.attributes.find("TANGENT");
    if (tanIt != prim.attributes.end()) {
        tangents = readVec4AsVec3Accessor(model, tanIt->second);
        if (tangents.size() != n) tangents.clear();
    }

    auto indices = readIndicesAccessor(model, prim.indices);
    if (indices.empty()) {
        Log::error("GltfLoader: index accessor read failed");
        return std::nullopt;
    }

    MeshData out;
    out.vertices.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Vertex v;
        v.position = positions[i];
        v.normal   = normals[i];
        v.uv       = (i < uvs.size())      ? uvs[i]      : Vec2{0.0f, 0.0f};
        v.tangent  = (i < tangents.size()) ? tangents[i] : Vec3{1.0f, 0.0f, 0.0f};
        out.vertices.push_back(v);
    }
    out.indices = std::move(indices);

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

    GltfModel result;
    result.mesh = std::move(out);
    result.materialPaths = std::move(matPaths);

    Log::info("GltfLoader: loaded %s - %zu verts, %zu indices",
              path.c_str(), result.mesh.vertices.size(), result.mesh.indices.size());
    return result;
}

std::optional<MeshData> loadGltfMesh(const std::string& path) {
    auto model = loadGltfModel(path);
    if (!model) return std::nullopt;
    return std::move(model->mesh);
}

}  // namespace iron
