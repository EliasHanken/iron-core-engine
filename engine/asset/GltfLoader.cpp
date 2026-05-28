// GltfLoader.cpp -- tinygltf-backed loader for a single static primitive.
// All tinygltf types live in this translation unit; the public header
// only exposes engine types (MeshData, std::optional, std::string).

#include "asset/Animation.h"
#include "asset/GltfLoader.h"
#include "core/Log.h"
#include "math/Transform.h"   // for iron::translation

// tinygltf needs these defines in exactly one TU before its header.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <utility>

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

// Read a vec4 of u8 or u16 (JOINTS_0).
std::vector<std::array<std::uint32_t, 4>> readJointsAccessor(
    const tinygltf::Model& model, int accessorIdx) {
    std::vector<std::array<std::uint32_t, 4>> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.type != TINYGLTF_TYPE_VEC4) return out;
    if (acc.bufferView < 0 ||
        acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return out;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf  = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base = buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * byteStride;
        std::array<std::uint32_t, 4> j{};
        switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                j[0] = p[0]; j[1] = p[1]; j[2] = p[2]; j[3] = p[3];
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                std::uint16_t tmp[4];
                std::memcpy(tmp, p, sizeof(tmp));
                j[0] = tmp[0]; j[1] = tmp[1]; j[2] = tmp[2]; j[3] = tmp[3];
            } break;
            default: return {};
        }
        out.push_back(j);
    }
    return out;
}

// Read a vec4 of float (WEIGHTS_0).
std::vector<std::array<float, 4>> readVec4FloatAccessor(
    const tinygltf::Model& model, int accessorIdx) {
    std::vector<std::array<float, 4>> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
    if (acc.type != TINYGLTF_TYPE_VEC4) return out;
    if (acc.bufferView < 0 ||
        acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return out;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf  = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base = buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * byteStride;
        std::array<float, 4> w{};
        std::memcpy(w.data(), p, sizeof(w));
        out.push_back(w);
    }
    return out;
}

// Read a mat4-float accessor (inverseBindMatrices). glTF stores
// mat4s as 16 floats column-major - matches iron::Mat4 storage.
std::vector<Mat4> readMat4Accessor(const tinygltf::Model& model, int accessorIdx) {
    std::vector<Mat4> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const auto& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
    if (acc.type != TINYGLTF_TYPE_MAT4) return out;
    if (acc.bufferView < 0 ||
        acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return out;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf  = model.buffers[view.buffer];
    const std::size_t byteStride = acc.ByteStride(view);
    const unsigned char* base = buf.data.data() + view.byteOffset + acc.byteOffset;
    out.reserve(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * byteStride;
        Mat4 m;
        std::memcpy(&m, p, sizeof(Mat4));
        out.push_back(m);
    }
    return out;
}

// M24: read a float accessor of any SCALAR/VEC2/VEC3/VEC4 type into a
// packed std::vector<float>. Used by animation sampler inputs (SCALAR
// timestamps) and outputs (VEC3 for T/S, VEC4 for R). Component count
// is implicit in `accessor.type` and the result has `count * comps`
// floats.
bool readPackedFloatAccessor(const tinygltf::Model& m, int accessorIndex,
                             std::vector<float>& out) {
    if (accessorIndex < 0 ||
        accessorIndex >= static_cast<int>(m.accessors.size())) {
        return false;
    }
    const auto& a = m.accessors[accessorIndex];
    if (a.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
    int comps = 0;
    switch (a.type) {
        case TINYGLTF_TYPE_SCALAR: comps = 1; break;
        case TINYGLTF_TYPE_VEC2:   comps = 2; break;
        case TINYGLTF_TYPE_VEC3:   comps = 3; break;
        case TINYGLTF_TYPE_VEC4:   comps = 4; break;
        default: return false;
    }
    if (a.bufferView < 0 ||
        a.bufferView >= static_cast<int>(m.bufferViews.size())) {
        return false;
    }
    const auto& bv  = m.bufferViews[a.bufferView];
    const auto& buf = m.buffers[bv.buffer];
    const std::size_t elemBytes = static_cast<std::size_t>(comps) * sizeof(float);
    const std::size_t stride    = a.ByteStride(bv) ? static_cast<std::size_t>(a.ByteStride(bv))
                                                   : elemBytes;
    const std::uint8_t* base =
        buf.data.data() + bv.byteOffset + a.byteOffset;
    out.resize(a.count * static_cast<std::size_t>(comps));
    for (std::size_t i = 0; i < a.count; ++i) {
        std::memcpy(&out[i * comps], base + i * stride, elemBytes);
    }
    return true;
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

    // Walk scene -> node DFS to find the first mesh (M23: depth-unbounded;
    // RiggedSimple buries the mesh under Z_UP -> Armature -> Cylinder).
    int meshIdx = -1;
    int hostNodeIdx = -1;
    {
        std::vector<int> stack(scene.nodes.rbegin(), scene.nodes.rend());
        while (!stack.empty()) {
            const int nodeIdx = stack.back();
            stack.pop_back();
            if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) continue;
            const auto& node = model.nodes[nodeIdx];
            if (node.mesh >= 0) {
                meshIdx = node.mesh;
                hostNodeIdx = nodeIdx;
                break;
            }
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
                stack.push_back(*it);
            }
        }
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

    // M23 - skinned mesh data. Populated only if the primitive has
    // JOINTS_0/WEIGHTS_0 AND the host node references a skin.
    //
    // M24 also needs a node-index -> bone-index map to resolve animation
    // channel targets. It's populated below alongside skeleton construction.
    std::optional<SkinnedMeshData> skinnedMesh;
    std::unordered_map<int, int>   nodeToBone;
    auto jointsIt  = prim.attributes.find("JOINTS_0");
    auto weightsIt = prim.attributes.find("WEIGHTS_0");
    const bool hasSkinAttrs =
        (jointsIt != prim.attributes.end()) && (weightsIt != prim.attributes.end());

    if (hasSkinAttrs && hostNodeIdx >= 0 &&
        hostNodeIdx < static_cast<int>(model.nodes.size())) {
        const auto& hostNode = model.nodes[hostNodeIdx];
        if (hostNode.skin >= 0 &&
            hostNode.skin < static_cast<int>(model.skins.size())) {
            const auto& skin = model.skins[hostNode.skin];

            const auto jointTuples  = readJointsAccessor (model, jointsIt->second);
            const auto weightTuples = readVec4FloatAccessor(model, weightsIt->second);
            const auto invBinds     = readMat4Accessor   (model, skin.inverseBindMatrices);

            const std::size_t nVerts = positions.size();
            if (jointTuples.size() == nVerts &&
                weightTuples.size() == nVerts &&
                invBinds.size() == skin.joints.size()) {

                SkinnedMeshData sm;
                sm.indices = out.indices;  // same index buffer as the MeshData

                sm.vertices.reserve(nVerts);
                for (std::size_t i = 0; i < nVerts; ++i) {
                    SkinnedVertex sv;
                    sv.position = positions[i];
                    sv.normal   = normals[i];
                    sv.uv       = (i < uvs.size())      ? uvs[i]      : Vec2{0,0};
                    sv.tangent  = (i < tangents.size()) ? tangents[i] : Vec3{1,0,0};
                    sv.joints[0]  = jointTuples[i][0];
                    sv.joints[1]  = jointTuples[i][1];
                    sv.joints[2]  = jointTuples[i][2];
                    sv.joints[3]  = jointTuples[i][3];
                    // Normalize weights (in case glTF asset isn't strict).
                    float wsum = weightTuples[i][0] + weightTuples[i][1]
                               + weightTuples[i][2] + weightTuples[i][3];
                    if (wsum < 1e-6f) wsum = 1.0f;
                    sv.weights[0] = weightTuples[i][0] / wsum;
                    sv.weights[1] = weightTuples[i][1] / wsum;
                    sv.weights[2] = weightTuples[i][2] / wsum;
                    sv.weights[3] = weightTuples[i][3] / wsum;
                    sm.vertices.push_back(sv);
                }

                // Build the Skeleton: one Bone per skin.joints entry.
                // Also populate nodeToBone so animation channels can resolve
                // their target node to a bone index.
                sm.skeleton.bones.reserve(skin.joints.size());
                for (std::size_t i = 0; i < skin.joints.size(); ++i) {
                    const int jointNodeIdx = skin.joints[i];
                    if (jointNodeIdx >= 0) {
                        nodeToBone.emplace(jointNodeIdx, static_cast<int>(i));
                    }
                    Bone b;
                    b.inverseBindMatrix  = invBinds[i];
                    b.localBindTransform = Mat4::identity();
                    b.parentIndex        = -1;
                    if (jointNodeIdx >= 0 &&
                        jointNodeIdx < static_cast<int>(model.nodes.size())) {
                        const auto& jnode = model.nodes[jointNodeIdx];
                        b.name = jnode.name;
                        // Build localBindTransform from the node's TRS or matrix.
                        if (jnode.matrix.size() == 16) {
                            // glTF spec: matrices stored column-major. Same as iron::Mat4.
                            for (int r = 0; r < 4; ++r) {
                                for (int c = 0; c < 4; ++c) {
                                    b.localBindTransform.at(r, c) =
                                        static_cast<float>(jnode.matrix[c * 4 + r]);
                                }
                            }
                        } else {
                            // TRS path: only translation in v1.
                            // TODO(M24): apply rotation (quaternion) + scale.
                            Mat4 t = Mat4::identity();
                            if (jnode.translation.size() == 3) {
                                t = iron::translation(Vec3{
                                    static_cast<float>(jnode.translation[0]),
                                    static_cast<float>(jnode.translation[1]),
                                    static_cast<float>(jnode.translation[2])});
                            }
                            b.localBindTransform = t;
                        }
                    }
                    sm.skeleton.bones.push_back(b);
                }
                // Resolve parent indices by walking each joint node's children.
                for (std::size_t i = 0; i < skin.joints.size(); ++i) {
                    const int jointNodeIdx = skin.joints[i];
                    if (jointNodeIdx < 0 ||
                        jointNodeIdx >= static_cast<int>(model.nodes.size())) continue;
                    for (const int childNodeIdx : model.nodes[jointNodeIdx].children) {
                        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                            if (skin.joints[j] == childNodeIdx) {
                                sm.skeleton.bones[j].parentIndex = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                }

                skinnedMesh = std::move(sm);
            }
        }
    }

    // --- M24: animations ---------------------------------------------------
    // Parse every animation clip. Channels whose target node isn't in
    // nodeToBone get targetBone = -1 (player skips them). CUBICSPLINE
    // samplers are downgraded to LINEAR with a one-time warning;
    // 'weights' (morph) channels are skipped with a one-time warning.
    std::vector<AnimationClip> animations;
    {
        bool warnedCubicSpline = false;
        bool warnedWeightsPath = false;
        animations.reserve(model.animations.size());
        for (const auto& gltfAnim : model.animations) {
            AnimationClip clip;
            clip.name = gltfAnim.name;

            clip.samplers.reserve(gltfAnim.samplers.size());
            for (const auto& gs : gltfAnim.samplers) {
                AnimationSampler samp;
                if (gs.interpolation == "STEP") {
                    samp.interpolation = AnimationInterpolation::Step;
                } else if (gs.interpolation == "CUBICSPLINE") {
                    samp.interpolation = AnimationInterpolation::Linear;
                    if (!warnedCubicSpline) {
                        Log::warn("GltfLoader: CUBICSPLINE interpolation "
                                  "not supported; downgrading to LINEAR");
                        warnedCubicSpline = true;
                    }
                } else {
                    samp.interpolation = AnimationInterpolation::Linear;
                }
                // On failure we still push an empty sampler so that channel
                // samplerIndex values (which are absolute indices from the
                // glTF file) stay aligned with clip.samplers. The player's
                // sample functions early-return identity/zero for empty
                // samplers, so this is safe.
                if (!readPackedFloatAccessor(model, gs.input, samp.inputs)) {
                    Log::warn("GltfLoader: animation sampler has unreadable "
                              "input accessor; keeping empty sampler to "
                              "preserve channel indices");
                    samp.inputs.clear();
                    samp.outputs.clear();
                    clip.samplers.push_back(std::move(samp));
                    continue;
                }
                if (!readPackedFloatAccessor(model, gs.output, samp.outputs)) {
                    Log::warn("GltfLoader: animation sampler has unreadable "
                              "output accessor; keeping empty sampler to "
                              "preserve channel indices");
                    samp.inputs.clear();
                    samp.outputs.clear();
                    clip.samplers.push_back(std::move(samp));
                    continue;
                }
                clip.samplers.push_back(std::move(samp));
            }

            clip.channels.reserve(gltfAnim.channels.size());
            for (const auto& gc : gltfAnim.channels) {
                AnimationChannel ch;
                ch.samplerIndex = gc.sampler;

                const std::string& p = gc.target_path;
                if (p == "translation") {
                    ch.path = AnimationPath::Translation;
                } else if (p == "rotation") {
                    ch.path = AnimationPath::Rotation;
                } else if (p == "scale") {
                    ch.path = AnimationPath::Scale;
                } else if (p == "weights") {
                    if (!warnedWeightsPath) {
                        Log::warn("GltfLoader: animation 'weights' path "
                                  "not supported; skipping channel");
                        warnedWeightsPath = true;
                    }
                    continue;
                } else {
                    Log::warn("GltfLoader: animation has unknown target "
                              "path '%s'; skipping channel", p.c_str());
                    continue;
                }

                auto it = nodeToBone.find(gc.target_node);
                ch.targetBone = (it != nodeToBone.end()) ? it->second : -1;
                clip.channels.push_back(ch);
            }

            clip.duration = 0.0f;
            for (const auto& samp : clip.samplers) {
                if (!samp.inputs.empty()) {
                    clip.duration = std::max(clip.duration, samp.inputs.back());
                }
            }

            animations.push_back(std::move(clip));
        }
    }

    GltfModel result;
    result.mesh = std::move(out);
    result.materialPaths = std::move(matPaths);
    result.skinnedMesh = std::move(skinnedMesh);
    result.animations = std::move(animations);

    Log::info("GltfLoader: loaded %s - %zu verts, %zu indices%s (%zu anim%s)",
              path.c_str(), result.mesh.vertices.size(), result.mesh.indices.size(),
              result.skinnedMesh.has_value() ? " (skinned)" : "",
              result.animations.size(),
              result.animations.size() == 1 ? "" : "s");
    return result;
}

std::optional<MeshData> loadGltfMesh(const std::string& path) {
    auto model = loadGltfModel(path);
    if (!model) return std::nullopt;
    return std::move(model->mesh);
}

const AnimationClip* GltfModel::findClip(std::string_view name) const {
    for (const auto& clip : animations) {
        if (clip.name == name) return &clip;
    }
    return nullptr;
}

}  // namespace iron
