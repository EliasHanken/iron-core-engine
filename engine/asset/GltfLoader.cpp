// GltfLoader.cpp -- tinygltf-backed loader for a single static primitive.
// All tinygltf types live in this translation unit; the public header
// only exposes engine types (MeshData, std::optional, std::string).

#include "asset/Animation.h"
#include "asset/GltfLoader.h"
#include "core/Log.h"
#include "math/Quaternion.h"  // for iron::Quat (bone TRS rotation)
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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
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

// M25 Task 3a: synthesize an identity index buffer (0, 1, 2, ..., n-1)
// for non-indexed primitives. glTF allows omitting `indices`; this lets the
// rest of the engine assume an indexed pipeline without a separate
// non-indexed path.
std::vector<std::uint32_t> synthesizeSequentialIndices(std::size_t vertexCount) {
    std::vector<std::uint32_t> out(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        out[i] = static_cast<std::uint32_t>(i);
    }
    return out;
}

// M25 Task 3a: compute per-vertex normals by summing each incident
// triangle's face normal and normalizing. For non-indexed meshes (where
// every vertex belongs to exactly one triangle) this degenerates to flat
// shading - which is what you want when the source asset deliberately
// omits NORMAL.
std::vector<Vec3> computeNormalsFromIndices(const std::vector<Vec3>& positions,
                                            const std::vector<std::uint32_t>& indices) {
    std::vector<Vec3> normals(positions.size(), Vec3{0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t i0 = indices[i + 0];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        if (i0 >= positions.size() || i1 >= positions.size() ||
            i2 >= positions.size()) {
            continue;
        }
        const Vec3 e1 = positions[i1] - positions[i0];
        const Vec3 e2 = positions[i2] - positions[i0];
        const Vec3 faceNormal = cross(e1, e2);
        normals[i0] = normals[i0] + faceNormal;
        normals[i1] = normals[i1] + faceNormal;
        normals[i2] = normals[i2] + faceNormal;
    }
    for (auto& n : normals) {
        const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len2 > 1e-12f) {
            const float inv = 1.0f / std::sqrt(len2);
            n.x *= inv;
            n.y *= inv;
            n.z *= inv;
        } else {
            n = Vec3{0.0f, 1.0f, 0.0f};  // degenerate triangle fallback
        }
    }
    return normals;
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

// One Bone per skin joint, with each joint node's local transform as its bind
// transform and parents resolved through the node tree. Records which node
// became which bone, so animation channels can find their targets.
Skeleton buildSkeleton(const tinygltf::Model& model, const tinygltf::Skin& skin,
                       std::unordered_map<int, int>& nodeToBone) {
    Skeleton skeleton;
    const auto invBinds = readMat4Accessor(model, skin.inverseBindMatrices);
    // Build the Skeleton: one Bone per skin.joints entry.
    // Also populate nodeToBone so animation channels can resolve
    // their target node to a bone index.
    skeleton.bones.reserve(skin.joints.size());
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
                // TRS path. Compose translation × rotation × scale in column-major
                // form. glTF stores rotation as a unit quaternion (xyzw) and scale
                // as a vec3. Missing components default to identity.
                Vec3 t{0, 0, 0};
                if (jnode.translation.size() == 3) {
                    t = Vec3{
                        static_cast<float>(jnode.translation[0]),
                        static_cast<float>(jnode.translation[1]),
                        static_cast<float>(jnode.translation[2])};
                }
                Quat q = Quat::identity();
                if (jnode.rotation.size() == 4) {
                    q = Quat{
                        static_cast<float>(jnode.rotation[0]),
                        static_cast<float>(jnode.rotation[1]),
                        static_cast<float>(jnode.rotation[2]),
                        static_cast<float>(jnode.rotation[3])};
                }
                Vec3 s{1, 1, 1};
                if (jnode.scale.size() == 3) {
                    s = Vec3{
                        static_cast<float>(jnode.scale[0]),
                        static_cast<float>(jnode.scale[1]),
                        static_cast<float>(jnode.scale[2])};
                }
                // M = T * R * S  (matches the column-major convention used by
                // AnimationPlayer's composeTRS).
                Mat4 R = q.toMat4();
                R.at(0, 0) *= s.x; R.at(1, 0) *= s.x; R.at(2, 0) *= s.x;
                R.at(0, 1) *= s.y; R.at(1, 1) *= s.y; R.at(2, 1) *= s.y;
                R.at(0, 2) *= s.z; R.at(1, 2) *= s.z; R.at(2, 2) *= s.z;
                R.at(0, 3) = t.x; R.at(1, 3) = t.y; R.at(2, 3) = t.z;
                R.at(3, 3) = 1.0f;
                b.localBindTransform = R;
            }
        }
        skeleton.bones.push_back(b);
    }
    // Resolve parent indices by walking each joint node's children.
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const int jointNodeIdx = skin.joints[i];
        if (jointNodeIdx < 0 ||
            jointNodeIdx >= static_cast<int>(model.nodes.size())) continue;
        for (const int childNodeIdx : model.nodes[jointNodeIdx].children) {
            for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                if (skin.joints[j] == childNodeIdx) {
                    skeleton.bones[j].parentIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    return skeleton;
}

// Every animation clip. Channels whose target node isn't a bone get targetBone
// -1 (players skip them). CUBICSPLINE samplers are downgraded to LINEAR and
// 'weights' (morph) channels skipped, each with a one-time warning.
std::vector<AnimationClip> readAnimations(const tinygltf::Model& model,
                                          const std::unordered_map<int, int>& nodeToBone) {
    std::vector<AnimationClip> animations;
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
    return animations;
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
    if (posIt == prim.attributes.end()) {
        Log::error("GltfLoader: primitive missing POSITION");
        return std::nullopt;
    }
    const auto positions = readVec3Accessor(model, posIt->second);
    if (positions.empty()) {
        Log::error("GltfLoader: position accessor read failed or empty");
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

    // M25 Task 3a: indices may be absent (non-indexed primitives are legal
    // per the glTF spec, e.g. Fox.glb). Read if present, synthesize
    // 0..n-1 otherwise. We need indices BEFORE normals because the
    // flat-shading fallback walks triangles via the index buffer.
    std::vector<std::uint32_t> indices;
    if (prim.indices >= 0) {
        indices = readIndicesAccessor(model, prim.indices);
        if (indices.empty()) {
            Log::error("GltfLoader: index accessor present but unreadable");
            return std::nullopt;
        }
    } else {
        indices = synthesizeSequentialIndices(n);
        Log::info(
            "GltfLoader: synthesized sequential index buffer for non-indexed "
            "primitive (%zu verts)",
            n);
    }

    // M25 Task 3a: NORMAL may be absent (exporters frequently skip it).
    // Read if present, compute flat-shaded normals from positions + indices
    // otherwise. If the accessor's count mismatches POSITION we also
    // recompute - the source is broken either way and a recomputed buffer
    // beats a malformed one.
    auto nrmIt = prim.attributes.find("NORMAL");
    std::vector<Vec3> normals;
    if (nrmIt != prim.attributes.end()) {
        normals = readVec3Accessor(model, nrmIt->second);
        if (normals.size() != n) {
            Log::warn(
                "GltfLoader: NORMAL accessor count %zu mismatches POSITION "
                "count %zu; recomputing",
                normals.size(), n);
            normals = computeNormalsFromIndices(positions, indices);
        }
    } else {
        Log::info(
            "GltfLoader: NORMAL absent; computing flat-shaded normals from "
            "geometry");
        normals = computeNormalsFromIndices(positions, indices);
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
    // M45c — PBR scalar factors; defaults match glTF spec (metallic/roughness=1,
    // baseColor={1,1,1}, emissive={0,0,0}) and are only overridden when a
    // material is present.
    GltfMaterialPaths matPaths;
    float matMetallicFactor  = 1.0f;
    float matRoughnessFactor = 1.0f;
    Vec3  matBaseColorFactor{1.0f, 1.0f, 1.0f};
    Vec3  matEmissiveFactor{0.0f, 0.0f, 0.0f};
    float matNormalScale     = 1.0f;
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
        matPaths.occlusion      = resolve(mat.occlusionTexture.index);
        matPaths.emissive       = resolve(mat.emissiveTexture.index);

        matMetallicFactor  = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
        matRoughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
        const auto& bcf = mat.pbrMetallicRoughness.baseColorFactor;  // size 4, default {1,1,1,1}
        if (bcf.size() >= 3)
            matBaseColorFactor = {static_cast<float>(bcf[0]),
                                  static_cast<float>(bcf[1]),
                                  static_cast<float>(bcf[2])};
        const auto& ef = mat.emissiveFactor;                         // size 3, default {0,0,0}
        if (ef.size() >= 3)
            matEmissiveFactor = {static_cast<float>(ef[0]),
                                 static_cast<float>(ef[1]),
                                 static_cast<float>(ef[2])};
        matNormalScale = static_cast<float>(mat.normalTexture.scale);
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

                sm.skeleton = buildSkeleton(model, skin, nodeToBone);

                skinnedMesh = std::move(sm);
            }
        }
    }

    // --- M24: animations ---------------------------------------------------
    // Parse every animation clip. Channels whose target node isn't in
    // nodeToBone get targetBone = -1 (player skips them). CUBICSPLINE
    // samplers are downgraded to LINEAR with a one-time warning;
    // 'weights' (morph) channels are skipped with a one-time warning.
    std::vector<AnimationClip> animations = readAnimations(model, nodeToBone);

    GltfModel result;
    result.mesh = std::move(out);
    result.materialPaths  = std::move(matPaths);
    result.skinnedMesh    = std::move(skinnedMesh);
    result.animations     = std::move(animations);
    result.metallicFactor  = matMetallicFactor;
    result.roughnessFactor = matRoughnessFactor;
    result.baseColorFactor = matBaseColorFactor;
    result.emissiveFactor  = matEmissiveFactor;
    result.normalScale     = matNormalScale;

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

namespace {

// A node's own transform, from its matrix or its translation, rotation and scale.
Mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        Mat4 m;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) m.at(r, c) = static_cast<float>(node.matrix[c * 4 + r]);
        }
        return m;
    }
    Mat4 t = Mat4::identity();
    if (node.translation.size() == 3) {
        t = translation({static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                         static_cast<float>(node.translation[2])});
    }
    Mat4 r = Mat4::identity();
    if (node.rotation.size() == 4) {
        r = Quat{static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]),
                 static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3])}.toMat4();
    }
    Mat4 s = Mat4::identity();
    if (node.scale.size() == 3) {
        s = scaling({static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                     static_cast<float>(node.scale[2])});
    }
    return t * r * s;
}

// Tangents along +U, per vertex, from each triangle's UV gradient.
std::vector<Vec3> computeTangents(const std::vector<Vec3>& p, const std::vector<Vec3>& n,
                                  const std::vector<Vec2>& uv, const std::vector<std::uint32_t>& idx) {
    std::vector<Vec3> t(p.size(), Vec3{0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
        const std::uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= p.size() || b >= p.size() || c >= p.size()) continue;
        const Vec3 e1 = p[b] - p[a];
        const Vec3 e2 = p[c] - p[a];
        const float du1 = uv[b].x - uv[a].x, dv1 = uv[b].y - uv[a].y;
        const float du2 = uv[c].x - uv[a].x, dv2 = uv[c].y - uv[a].y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-12f) continue;
        const Vec3 tangent = (e1 * dv2 - e2 * dv1) * (1.0f / det);
        t[a] = t[a] + tangent;
        t[b] = t[b] + tangent;
        t[c] = t[c] + tangent;
    }
    for (std::size_t i = 0; i < t.size(); ++i) {
        // Gram-Schmidt against the normal, with a fallback for untextured faces.
        Vec3 v = t[i] - n[i] * dot(n[i], t[i]);
        if (dot(v, v) < 1e-12f) {
            const Vec3 helper = std::abs(n[i].x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
            v = cross(helper, n[i]);
        }
        t[i] = normalize(v);
    }
    return t;
}

// Reads a glTF file's structure; images are left for the caller to load by path.
bool parseFile(const std::string& path, tinygltf::Model& model) {
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    loader.SetImageLoader(
        [](tinygltf::Image*, const int, std::string*, std::string*, int, int,
           const unsigned char*, int, void*) { return true; },
        nullptr);
    const std::string ext = std::filesystem::path(path).extension().string();
    const bool ok = (ext == ".glb" || ext == ".GLB") ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                                                     : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty()) Log::warn("GltfLoader: %s", warn.c_str());
    if (!ok || model.scenes.empty()) {
        Log::error("GltfLoader: could not read %s: %s", path.c_str(), err.c_str());
        return false;
    }
    return true;
}

// A material's texture paths (resolved next to the file) and PBR factors. Embedded
// (data:) images come back as empty paths.
void readSurface(const tinygltf::Model& model, int materialIndex, const std::filesystem::path& dir,
                 GltfSurface& out) {
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size())) return;
    const auto resolve = [&](int textureIndex) -> std::string {
        if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) return {};
        const auto& tex = model.textures[textureIndex];
        if (tex.source < 0 || tex.source >= static_cast<int>(model.images.size())) return {};
        const auto& img = model.images[tex.source];
        if (img.uri.empty() || img.uri.substr(0, 5) == "data:") return {};
        return (dir / img.uri).string();
    };
    const auto& mat = model.materials[materialIndex];
    out.materialPaths.albedo = resolve(mat.pbrMetallicRoughness.baseColorTexture.index);
    out.materialPaths.normal = resolve(mat.normalTexture.index);
    out.materialPaths.metalRoughness = resolve(mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
    out.materialPaths.occlusion = resolve(mat.occlusionTexture.index);
    out.materialPaths.emissive = resolve(mat.emissiveTexture.index);
    out.metallicFactor = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    out.roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
    const auto& bcf = mat.pbrMetallicRoughness.baseColorFactor;
    if (bcf.size() >= 3) {
        out.baseColorFactor = {static_cast<float>(bcf[0]), static_cast<float>(bcf[1]), static_cast<float>(bcf[2])};
    }
    const auto& ef = mat.emissiveFactor;
    if (ef.size() >= 3) {
        out.emissiveFactor = {static_cast<float>(ef[0]), static_cast<float>(ef[1]), static_cast<float>(ef[2])};
    }
    out.normalScale = static_cast<float>(mat.normalTexture.scale);
}

}  // namespace

std::optional<GltfStaticModel> loadGltfStaticModel(const std::string& path, const GltfStaticOptions& options) {
    tinygltf::Model model;
    if (!parseFile(path, model)) return std::nullopt;
    const int sceneIdx = model.defaultScene >= 0 && model.defaultScene < static_cast<int>(model.scenes.size())
                             ? model.defaultScene : 0;
    const std::filesystem::path dir = std::filesystem::absolute(path).parent_path();

    GltfStaticModel out;
    std::map<std::pair<std::string, int>, std::size_t> slots;  // (node, material) -> section
    bool first = true;

    // Depth first, carrying each node's world transform.
    std::vector<std::pair<int, Mat4>> stack;
    for (auto it = model.scenes[sceneIdx].nodes.rbegin(); it != model.scenes[sceneIdx].nodes.rend(); ++it) {
        stack.emplace_back(*it, Mat4::identity());
    }
    while (!stack.empty()) {
        const auto [nodeIdx, parent] = stack.back();
        stack.pop_back();
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) continue;
        const auto& node = model.nodes[nodeIdx];
        const Mat4 world = parent * nodeLocalMatrix(node);
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) stack.emplace_back(*it, world);
        if (node.mesh < 0 || node.mesh >= static_cast<int>(model.meshes.size())) continue;

        // Normals go through the inverse transpose, so that non-uniform scale keeps them square.
        const Mat4 normalMatrix = transpose(inverse(world));
        for (const auto& prim : model.meshes[node.mesh].primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;
            const auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end()) continue;
            const auto positions = readVec3Accessor(model, posIt->second);
            if (positions.empty()) continue;
            const std::size_t n = positions.size();
            std::vector<std::uint32_t> indices = prim.indices >= 0 ? readIndicesAccessor(model, prim.indices)
                                                                   : synthesizeSequentialIndices(n);
            if (indices.empty()) continue;
            std::vector<Vec3> normals;
            if (const auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end()) {
                normals = readVec3Accessor(model, it->second);
            }
            if (normals.size() != n) normals = computeNormalsFromIndices(positions, indices);
            std::vector<Vec2> uvs;
            if (const auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end()) {
                uvs = readVec2Accessor(model, it->second);
            }
            if (uvs.size() != n) uvs.assign(n, Vec2{0.0f, 0.0f});
            std::vector<Vec3> tangents;
            if (const auto it = prim.attributes.find("TANGENT"); it != prim.attributes.end()) {
                tangents = readVec4AsVec3Accessor(model, it->second);
            }
            if (tangents.size() != n) tangents = computeTangents(positions, normals, uvs, indices);

            const auto key = std::make_pair(options.splitByNode ? node.name : std::string{}, prim.material);
            auto slot = slots.find(key);
            if (slot == slots.end()) {
                GltfSection section;
                section.node = key.first;
                readSurface(model, prim.material, dir, section);
                out.sections.push_back(std::move(section));
                slot = slots.emplace(key, out.sections.size() - 1).first;
            }

            MeshData& mesh = out.sections[slot->second].mesh;
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (std::size_t i = 0; i < n; ++i) {
                const Vec4 p = world * Vec4{positions[i].x, positions[i].y, positions[i].z, 1.0f};
                const Vec4 nn = normalMatrix * Vec4{normals[i].x, normals[i].y, normals[i].z, 0.0f};
                const Vec4 tt = world * Vec4{tangents[i].x, tangents[i].y, tangents[i].z, 0.0f};
                Vertex v;
                v.position = {p.x, p.y, p.z};
                v.normal = normalize(Vec3{nn.x, nn.y, nn.z});
                v.tangent = normalize(Vec3{tt.x, tt.y, tt.z});
                v.uv = uvs[i];
                mesh.vertices.push_back(v);
                if (first) {
                    out.bounds = {v.position, v.position};
                    first = false;
                }
                out.bounds.min = {std::min(out.bounds.min.x, p.x), std::min(out.bounds.min.y, p.y),
                                  std::min(out.bounds.min.z, p.z)};
                out.bounds.max = {std::max(out.bounds.max.x, p.x), std::max(out.bounds.max.y, p.y),
                                  std::max(out.bounds.max.z, p.z)};
            }
            // A mirrored node flips its triangles' winding, so flip it back.
            const bool mirrored = dot(cross(Vec3{world.at(0, 0), world.at(1, 0), world.at(2, 0)},
                                            Vec3{world.at(0, 1), world.at(1, 1), world.at(2, 1)}),
                                      Vec3{world.at(0, 2), world.at(1, 2), world.at(2, 2)}) < 0.0f;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                mesh.indices.push_back(base + indices[i]);
                mesh.indices.push_back(base + indices[mirrored ? i + 2 : i + 1]);
                mesh.indices.push_back(base + indices[mirrored ? i + 1 : i + 2]);
            }
        }
    }
    if (out.sections.empty()) {
        Log::error("GltfLoader: no triangle meshes in %s", path.c_str());
        return std::nullopt;
    }
    return out;
}

std::optional<GltfSkinnedModel> loadGltfSkinnedModel(const std::string& path) {
    tinygltf::Model model;
    if (!parseFile(path, model)) return std::nullopt;
    const std::filesystem::path dir = std::filesystem::absolute(path).parent_path();

    // The skinned mesh nodes, in scene order; the first one's skin is the model's.
    const int sceneIdx = model.defaultScene >= 0 && model.defaultScene < static_cast<int>(model.scenes.size())
                             ? model.defaultScene : 0;
    std::vector<int> skinnedNodes;
    std::vector<int> stack(model.scenes[sceneIdx].nodes.rbegin(), model.scenes[sceneIdx].nodes.rend());
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        if (idx < 0 || idx >= static_cast<int>(model.nodes.size())) continue;
        const auto& node = model.nodes[idx];
        if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size()) && node.skin >= 0 &&
            node.skin < static_cast<int>(model.skins.size())) {
            skinnedNodes.push_back(idx);
        }
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) stack.push_back(*it);
    }
    if (skinnedNodes.empty()) {
        Log::error("GltfLoader: nothing in %s is skinned", path.c_str());
        return std::nullopt;
    }
    const int skinIdx = model.nodes[skinnedNodes.front()].skin;
    const auto& skin = model.skins[skinIdx];
    if (readMat4Accessor(model, skin.inverseBindMatrices).size() != skin.joints.size()) {
        Log::error("GltfLoader: %s's skin has no inverse bind matrix per joint", path.c_str());
        return std::nullopt;
    }

    GltfSkinnedModel out;
    std::unordered_map<int, int> nodeToBone;
    out.skeleton = buildSkeleton(model, skin, nodeToBone);
    std::map<int, std::size_t> slots;  // material -> section

    for (const int idx : skinnedNodes) {
        const auto& node = model.nodes[idx];
        if (node.skin != skinIdx) {
            Log::warn("GltfLoader: %s: mesh '%s' has another skin; skipped", path.c_str(), node.name.c_str());
            continue;
        }
        for (const auto& prim : model.meshes[node.mesh].primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;
            const auto posIt = prim.attributes.find("POSITION");
            const auto jointsIt = prim.attributes.find("JOINTS_0");
            const auto weightsIt = prim.attributes.find("WEIGHTS_0");
            if (posIt == prim.attributes.end() || jointsIt == prim.attributes.end() ||
                weightsIt == prim.attributes.end()) {
                continue;
            }
            const auto positions = readVec3Accessor(model, posIt->second);
            const std::size_t n = positions.size();
            const auto joints = readJointsAccessor(model, jointsIt->second);
            const auto weights = readVec4FloatAccessor(model, weightsIt->second);
            if (n == 0 || joints.size() != n || weights.size() != n) continue;
            std::vector<std::uint32_t> indices = prim.indices >= 0 ? readIndicesAccessor(model, prim.indices)
                                                                   : synthesizeSequentialIndices(n);
            if (indices.empty()) continue;
            std::vector<Vec3> normals;
            if (const auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end()) {
                normals = readVec3Accessor(model, it->second);
            }
            if (normals.size() != n) normals = computeNormalsFromIndices(positions, indices);
            std::vector<Vec2> uvs;
            if (const auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end()) {
                uvs = readVec2Accessor(model, it->second);
            }
            if (uvs.size() != n) uvs.assign(n, Vec2{0.0f, 0.0f});
            std::vector<Vec3> tangents;
            if (const auto it = prim.attributes.find("TANGENT"); it != prim.attributes.end()) {
                tangents = readVec4AsVec3Accessor(model, it->second);
            }
            if (tangents.size() != n) tangents = computeTangents(positions, normals, uvs, indices);

            auto slot = slots.find(prim.material);
            if (slot == slots.end()) {
                GltfSkinnedSection section;
                readSurface(model, prim.material, dir, section);
                out.sections.push_back(std::move(section));
                slot = slots.emplace(prim.material, out.sections.size() - 1).first;
            }
            SkinnedMeshData& mesh = out.sections[slot->second].mesh;
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (std::size_t i = 0; i < n; ++i) {
                SkinnedVertex v;
                v.position = positions[i];
                v.normal = normals[i];
                v.uv = uvs[i];
                v.tangent = tangents[i];
                float sum = weights[i][0] + weights[i][1] + weights[i][2] + weights[i][3];
                if (sum < 1e-6f) sum = 1.0f;
                for (int k = 0; k < 4; ++k) {
                    v.joints[k] = joints[i][k];
                    v.weights[k] = weights[i][k] / sum;
                }
                mesh.vertices.push_back(v);
            }
            for (const std::uint32_t index : indices) mesh.indices.push_back(base + index);
        }
    }
    if (out.sections.empty()) {
        Log::error("GltfLoader: %s has no skinned triangles", path.c_str());
        return std::nullopt;
    }
    out.animations = readAnimations(model, nodeToBone);
    Log::info("GltfLoader: loaded %s - %zu bones, %zu sections, %zu clips", path.c_str(),
              out.skeleton.bones.size(), out.sections.size(), out.animations.size());
    return out;
}

const AnimationClip* GltfSkinnedModel::findClip(std::string_view name) const {
    for (const auto& clip : animations) {
        if (clip.name == name) return &clip;
    }
    return nullptr;
}

int GltfSkinnedModel::findBone(std::string_view name) const {
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        if (skeleton.bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const AnimationClip* GltfModel::findClip(std::string_view name) const {
    for (const auto& clip : animations) {
        if (clip.name == name) return &clip;
    }
    return nullptr;
}

}  // namespace iron
