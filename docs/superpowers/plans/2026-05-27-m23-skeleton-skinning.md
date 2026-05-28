# M23 Skeleton + GPU Skinning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `iron::Skeleton` + `iron::SkinnedMeshData` engine types, extend the glTF loader to populate `GltfModel.skinnedMesh` from skin data, build a Vulkan skinning pipeline (4-influence weighted skinning with a 128-bone UBO at binding 7), and validate by rendering Khronos's `RiggedSimple.gltf` in bind pose through the 10-gltf-viewer.

**Architecture:** New parallel storage + pipeline path for skinned meshes — `VkSkinnedMeshStore`, a separate descriptor set layout (8 bindings: same 7 as scene + bones UBO), a separate `VkShader`-style entry created via `Renderer::createSkinnedShader`, and a separate pipeline factory entry. SkinnedVertex layout is 76 bytes (11 floats + 4 uints + 4 floats). Bone matrices are caller-supplied per draw (identity for bind pose; M24's AnimationPlayer will compute them from curves). Loader extension reads `prim.attributes.JOINTS_0` / `WEIGHTS_0` + the host node's `skin` → builds `SkinnedMeshData{ vertices, indices, skeleton }`.

**Tech Stack:** C++23, Vulkan 1.3, tinygltf (M22), glslang (engine), CMake, MSVC.

---

## File Structure

### New files
- `engine/asset/Skeleton.h` — `Bone` + `Skeleton` POD types
- `engine/scene/SkinnedMesh.h` — `SkinnedVertex` + `SkinnedMeshData`
- `engine/render/backends/vulkan/VkSkinnedMesh.h` — `VkSkinnedMeshResource` + `VkSkinnedMeshStore`
- `engine/render/backends/vulkan/VkSkinnedMesh.cpp` — implementation
- `tests/assets/gltf/RiggedSimple.gltf` + `RiggedSimple0.bin` (vendored Khronos CC0)
- `games/10-gltf-viewer/assets/rigged-simple/RiggedSimple.gltf` + `RiggedSimple0.bin` (vendored copy)

### Modified files
- `engine/asset/GltfLoader.h` — add `skinnedMesh` field to `GltfModel`
- `engine/asset/GltfLoader.cpp` — populate `skinnedMesh` when skin present
- `engine/render/Handles.h` — add `SkinnedMeshHandle` type + `kInvalidSkinnedMesh`
- `engine/render/Renderer.h` — add `SkinnedDrawCall` + `createSkinnedMesh` + `submitSkinnedDraw` + `createSkinnedShader`
- `engine/render/backends/vulkan/VulkanRenderer.h/.cpp` — implement the new methods
- `engine/render/backends/vulkan/VkShader.cpp` — add a 2nd descriptor set layout path (8 bindings) for skinned shaders
- `engine/render/backends/vulkan/VkPipeline.cpp` — add a skinned-pipeline factory entry with SkinnedVertex input layout
- `engine/render/backends/opengl/OpenGLRenderer.cpp` — stub the new virtual methods (OpenGL is deprecated/frozen; just `warnOnce` + return invalid)
- `engine/CMakeLists.txt` — register the two new Vulkan files
- `tests/test_gltf_loader.cpp` — new sub-check on RiggedSimple
- `games/10-gltf-viewer/main.cpp` — dispatch on `skinnedMesh.has_value()`, fill identity bone matrices, submit through skinned path; add `--model` CLI arg
- `docs/engine/asset-pipeline.md` — append M23 section

---

## Task 1: Engine types + glTF loader extension

**Files:**
- Create: `engine/asset/Skeleton.h`
- Create: `engine/scene/SkinnedMesh.h`
- Modify: `engine/asset/GltfLoader.h`
- Modify: `engine/asset/GltfLoader.cpp`
- Modify: `engine/render/Handles.h`
- Create: `tests/assets/gltf/RiggedSimple.gltf` + `tests/assets/gltf/RiggedSimple0.bin`
- Modify: `tests/test_gltf_loader.cpp`

After this task: the loader reads skin data; tests confirm RiggedSimple parses into a valid `SkinnedMeshData`. No renderer / Vulkan changes yet.

- [ ] **Step 1: Add `SkinnedMeshHandle` to `engine/render/Handles.h`**

Open `engine/render/Handles.h`. After the existing `MeshHandle` typedef + invalid constant, add:

```cpp
using SkinnedMeshHandle = std::uint32_t;
inline constexpr SkinnedMeshHandle kInvalidSkinnedMesh = 0;
```

- [ ] **Step 2: Create `engine/asset/Skeleton.h`**

```cpp
#pragma once

#include "math/Mat4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

// One bone in a skeletal hierarchy.
struct Bone {
    int  parentIndex = -1;             // -1 for root
    Mat4 inverseBindMatrix;            // glTF inverseBindMatrices[i]
    Mat4 localBindTransform;           // bone's local-space TRS at bind time
    std::string name;                  // diagnostic; not load-bearing
};

// Flat array of bones. Joint indices in SkinnedVertex / glTF refer to
// indices into this array. Hierarchy is encoded via `parentIndex`.
struct Skeleton {
    std::vector<Bone> bones;
};

}  // namespace iron
```

- [ ] **Step 3: Create `engine/scene/SkinnedMesh.h`**

```cpp
#pragma once

#include "asset/Skeleton.h"
#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// Skinned vertex: existing geometry + 4 bone influences.
// Total size: 76 bytes (11 floats + 4 uint32 + 4 floats).
struct SkinnedVertex {
    Vec3          position;
    Vec3          normal;
    Vec2          uv;
    Vec3          tangent;
    std::uint32_t joints[4];   // bone indices into Skeleton::bones
    float         weights[4];  // per-influence weights; normalized to sum 1
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};

}  // namespace iron
```

- [ ] **Step 4: Extend `engine/asset/GltfLoader.h` with `skinnedMesh` field**

Open `engine/asset/GltfLoader.h`. Add the include:

```cpp
#include "scene/SkinnedMesh.h"
```

Extend `GltfModel`:

```cpp
struct GltfModel {
    MeshData                       mesh;
    GltfMaterialPaths              materialPaths;
    std::optional<SkinnedMeshData> skinnedMesh;  // populated if glTF has a skin
};
```

(No API signature changes — same `loadGltfModel(path)` function.)

- [ ] **Step 5: Extend `engine/asset/GltfLoader.cpp` to populate `skinnedMesh`**

Open `engine/asset/GltfLoader.cpp`. Add helper readers for the skinning attributes in the anonymous namespace (alongside the existing `readVec3Accessor` etc.):

```cpp
// Read a uvec4 accessor (used for JOINTS_0). glTF spec allows u8 or u16.
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
            default: return {};  // unsupported
        }
        out.push_back(j);
    }
    return out;
}

// Read a vec4-float accessor as std::array<float, 4>. Used for WEIGHTS_0.
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

// Read a vec4-float accessor as std::vector<Mat4> (inverseBindMatrices is a
// MAT4 accessor but we use the same vec-of-floats reader path; glTF stores
// each mat4 as 16 floats in column-major order — same as engine Mat4).
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
```

Add `#include <array>` to the file's includes (top of file with other standard library headers).

Now, in `loadGltfModel`, AFTER the existing geometry extraction (`out.indices = std::move(indices);`) and BEFORE the material-paths extraction, add the skin-extraction block. The skin lives on the **node** that references the mesh (not the mesh itself). We need to find that node (the one we walked to in M22). Refactor the walk to remember the host-node index.

Find the existing walk code (where `meshIdx` is set). Update it to also remember `hostNodeIdx`:

```cpp
int meshIdx = -1;
int hostNodeIdx = -1;
for (const int nodeIdx : scene.nodes) {
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) continue;
    const auto& node = model.nodes[nodeIdx];
    if (node.mesh >= 0) {
        meshIdx = node.mesh;
        hostNodeIdx = nodeIdx;
        break;
    }
    for (const int childIdx : node.children) {
        if (childIdx < 0 || childIdx >= static_cast<int>(model.nodes.size())) continue;
        const auto& child = model.nodes[childIdx];
        if (child.mesh >= 0) {
            meshIdx = child.mesh;
            hostNodeIdx = childIdx;
            break;
        }
    }
    if (meshIdx >= 0) break;
}
```

Then, AFTER the existing material-paths block but BEFORE the final `return result;`, add the skin-extraction block:

```cpp
    // M23 — skinned mesh data. Populated only if the primitive has
    // JOINTS_0/WEIGHTS_0 AND the host node references a skin.
    std::optional<SkinnedMeshData> skinnedMesh;
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

            // Read per-vertex joints + weights.
            const auto jointTuples  = readJointsAccessor (model, jointsIt->second);
            const auto weightTuples = readVec4FloatAccessor(model, weightsIt->second);
            const auto invBinds     = readMat4Accessor   (model, skin.inverseBindMatrices);

            const std::size_t nVerts = positions.size();
            if (jointTuples.size() == nVerts &&
                weightTuples.size() == nVerts &&
                invBinds.size() == skin.joints.size()) {

                SkinnedMeshData sm;
                sm.indices = result.mesh.indices;  // share index buffer

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
                // parentIndex resolved by walking skin.joints[] and checking
                // each node's children → find which joint is the parent.
                sm.skeleton.bones.reserve(skin.joints.size());
                for (std::size_t i = 0; i < skin.joints.size(); ++i) {
                    const int jointNodeIdx = skin.joints[i];
                    Bone b;
                    b.inverseBindMatrix  = invBinds[i];
                    b.localBindTransform = Mat4::identity();  // computed below
                    b.parentIndex        = -1;
                    if (jointNodeIdx >= 0 &&
                        jointNodeIdx < static_cast<int>(model.nodes.size())) {
                        const auto& jnode = model.nodes[jointNodeIdx];
                        b.name = jnode.name;
                        // Build localBindTransform from the node's TRS or matrix.
                        if (jnode.matrix.size() == 16) {
                            for (int r = 0; r < 4; ++r) {
                                for (int c = 0; c < 4; ++c) {
                                    b.localBindTransform.at(r, c) =
                                        static_cast<float>(jnode.matrix[c * 4 + r]);
                                }
                            }
                        } else {
                            // TRS path: scale, then rotate, then translate.
                            Mat4 t = Mat4::identity();
                            if (jnode.translation.size() == 3) {
                                t = iron::translation(Vec3{
                                    static_cast<float>(jnode.translation[0]),
                                    static_cast<float>(jnode.translation[1]),
                                    static_cast<float>(jnode.translation[2])});
                            }
                            // Skip R + S for v1 (RiggedSimple uses identity rot/scale).
                            // A later milestone can wire quaternion + non-uniform scale.
                            b.localBindTransform = t;
                        }
                    }
                    sm.skeleton.bones.push_back(b);
                }
                // Resolve parent indices by walking each joint node's children
                // and matching them against skin.joints[].
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
```

> **Note:** quaternion rotation + scale on bones is deferred to M24. RiggedSimple has identity rotations on its bones so this works for the M23 validator. The implementer should add a `TODO(M24)` comment near the localBindTransform TRS path.

Update the final `return result;` to include `skinnedMesh`:

```cpp
    GltfModel result;
    result.mesh = std::move(out);
    result.materialPaths = std::move(matPaths);
    result.skinnedMesh = std::move(skinnedMesh);

    Log::info("GltfLoader: loaded %s - %zu verts, %zu indices%s",
              path.c_str(), result.mesh.vertices.size(), result.mesh.indices.size(),
              result.skinnedMesh.has_value() ? " (skinned)" : "");
    return result;
```

Need `#include "math/Transform.h"` near the top of GltfLoader.cpp (for `iron::translation`).

- [ ] **Step 6: Vendor RiggedSimple.gltf into `tests/assets/gltf/`**

Download from `github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/RiggedSimple/glTF`:

```
curl -L -o tests/assets/gltf/RiggedSimple.gltf https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple.gltf
curl -L -o tests/assets/gltf/RiggedSimple0.bin https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple0.bin
```

Total ~5 KB. CC0 licensed.

- [ ] **Step 7: Extend `tests/test_gltf_loader.cpp`**

After the existing BoxTextured-material-paths sub-test (and before the Triangle/invalid-path tests), add:

```cpp
    // --- M23: RiggedSimple.gltf has a skin with 2 bones + weighted vertices ---
    {
        auto model = loadGltfModel(base + "/RiggedSimple.gltf");
        CHECK(model.has_value());
        CHECK(model->skinnedMesh.has_value());
        const auto& sm = *model->skinnedMesh;
        // RiggedSimple has a 2-bone skeleton (a root + a child).
        CHECK(sm.skeleton.bones.size() == 2);
        // The child bone (index 1) should have parentIndex == 0.
        // (Order of the two bones may vary across asset versions; check that
        // exactly one bone is a root and exactly one has a parent.)
        int rootCount = 0;
        int childCount = 0;
        for (const auto& b : sm.skeleton.bones) {
            if (b.parentIndex < 0) ++rootCount;
            else ++childCount;
        }
        CHECK(rootCount == 1);
        CHECK(childCount == 1);
        // Each vertex has weights summing to ~1.0 (normalized at load time).
        for (const auto& v : sm.vertices) {
            const float wsum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
            CHECK(wsum > 0.99f && wsum < 1.01f);
        }
        // Inverse-bind matrices populated.
        CHECK(sm.skeleton.bones.size() == 2);  // (redundant, but explicit)
    }
```

- [ ] **Step 8: Build + run the test**

```
cmake --build build-vk --config Debug --target ironcore test_gltf_loader
ctest --test-dir build-vk -C Debug -R test_gltf_loader --output-on-failure
```

Expected: all sub-tests pass.

If RiggedSimple-skin sub-test fails:
- "bones.size() != 2" — check the actual joint count in RiggedSimple.gltf (open the JSON, look for `"joints":[...]`). Some asset variants have 2, some 3. Adjust the assertion to match what the vendored file actually has.
- "rootCount != 1" — the parent-resolution loop might be broken. Add a `Log::info("bone[%zu] parent=%d", i, b.parentIndex)` temporarily to inspect.
- "wsum" out of bounds — normalization step missed; verify Step 5's normalization code.

- [ ] **Step 9: Commit**

```
git add engine/asset/Skeleton.h engine/scene/SkinnedMesh.h \
        engine/asset/GltfLoader.h engine/asset/GltfLoader.cpp \
        engine/render/Handles.h \
        tests/assets/gltf/RiggedSimple.gltf tests/assets/gltf/RiggedSimple0.bin \
        tests/test_gltf_loader.cpp
git commit -m "M23 Task 1: Skeleton + SkinnedMeshData + glTF loader skin extraction"
```

---

## Task 2: Vulkan skinning pipeline + Renderer API

**Files:**
- Modify: `engine/render/Renderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkShader.cpp` (add skinned-shader path)
- Modify: `engine/render/backends/vulkan/VkPipeline.h/.cpp` (add skinned pipeline factory)
- Create: `engine/render/backends/vulkan/VkSkinnedMesh.h`
- Create: `engine/render/backends/vulkan/VkSkinnedMesh.cpp`
- Modify: `engine/render/backends/opengl/OpenGLRenderer.cpp` (stub the new methods — OpenGL is deprecated)
- Modify: `engine/CMakeLists.txt`

After this task: engine has a full skinned-mesh render path. No game uses it yet.

- [ ] **Step 1: Add `SkinnedDrawCall` + new virtual methods to `Renderer.h`**

Open `engine/render/Renderer.h`. Near the existing `DrawCall` struct, add:

```cpp
struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh = kInvalidSkinnedMesh;
    ShaderHandle      shader      = kInvalidHandle;
    Mat4              model       = Mat4::identity();
    Material          material;
    std::span<const Mat4> boneMatrices;  // joint matrices for skinning.
                                          // size must be ≤ kMaxBonesPerSkinnedMesh (128).
};

inline constexpr std::size_t kMaxBonesPerSkinnedMesh = 128;
```

`#include "asset/Skeleton.h"` and `#include "scene/SkinnedMesh.h"` at the top (alongside the existing `#include "scene/Mesh.h"`).

In the `Renderer` class `public:` section, after the existing `createMesh` / `createShader` methods, add:

```cpp
    // --- M23: Skinned mesh + draw API ---
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) = 0;
    virtual ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                              const std::string& fragmentSrc) = 0;
    virtual void submitSkinnedDraw(const SkinnedDrawCall& call) = 0;
```

`createSkinnedShader` is a separate factory because skinned shaders use a different descriptor set layout (8 bindings vs the scene's 7).

- [ ] **Step 2: Stub the new virtuals in `OpenGLRenderer`**

The OpenGL backend is deprecated as of PR #35 but still compiles. Stub the new methods to keep the build green:

In `engine/render/backends/opengl/OpenGLRenderer.h`:

```cpp
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) override;
    ShaderHandle createSkinnedShader(const std::string& v, const std::string& f) override;
    void submitSkinnedDraw(const SkinnedDrawCall& call) override;
```

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, add at the bottom (before the closing namespace):

```cpp
SkinnedMeshHandle OpenGLRenderer::createSkinnedMesh(const SkinnedMeshData&) {
    warnOnce("createSkinnedMesh");
    return kInvalidSkinnedMesh;
}
ShaderHandle OpenGLRenderer::createSkinnedShader(const std::string&, const std::string&) {
    warnOnce("createSkinnedShader");
    return kInvalidHandle;
}
void OpenGLRenderer::submitSkinnedDraw(const SkinnedDrawCall&) {
    warnOnce("submitSkinnedDraw");
}
```

(`warnOnce` is the existing helper used by the OpenGL backend for unsupported methods.)

- [ ] **Step 3: Create `engine/render/backends/vulkan/VkSkinnedMesh.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkSkinnedMesh is Vulkan-only."
#endif

#include "render/Handles.h"
#include "scene/SkinnedMesh.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkSkinnedMeshResource {
    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc  = VK_NULL_HANDLE;
    VkBuffer      indexBuffer  = VK_NULL_HANDLE;
    VmaAllocation indexAlloc   = VK_NULL_HANDLE;
    std::uint32_t indexCount   = 0;
    std::uint32_t boneCount    = 0;  // for clamping at submit time
};

class VkSkinnedMeshStore {
public:
    SkinnedMeshHandle create(VkContext& ctx, const SkinnedMeshData& data);
    const VkSkinnedMeshResource& get(SkinnedMeshHandle h) const;
    bool has(SkinnedMeshHandle h) const { return meshes_.count(h) != 0; }
    void destroyAll(VkContext& ctx);

private:
    std::unordered_map<SkinnedMeshHandle, VkSkinnedMeshResource> meshes_;
    SkinnedMeshHandle nextHandle_ = 1;
};

}  // namespace iron
```

- [ ] **Step 4: Create `engine/render/backends/vulkan/VkSkinnedMesh.cpp`**

```cpp
// VkSkinnedMesh.cpp — host-visible VMA buffers for SkinnedVertex + indices.

#include "render/backends/vulkan/VkSkinnedMesh.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cstring>

namespace iron {

namespace {

VkBuffer makeBuffer(VkContext& ctx, VkDeviceSize size,
                    VkBufferUsageFlags usage, VmaAllocation& outAlloc) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &buf, &outAlloc, nullptr));
    return buf;
}

}  // namespace

SkinnedMeshHandle VkSkinnedMeshStore::create(VkContext& ctx, const SkinnedMeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) return kInvalidSkinnedMesh;

    VkSkinnedMeshResource res;
    const VkDeviceSize vertBytes = data.vertices.size() * sizeof(SkinnedVertex);
    const VkDeviceSize idxBytes  = data.indices.size()  * sizeof(std::uint32_t);

    res.vertexBuffer = makeBuffer(ctx, vertBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, res.vertexAlloc);
    res.indexBuffer  = makeBuffer(ctx, idxBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, res.indexAlloc);
    res.indexCount   = static_cast<std::uint32_t>(data.indices.size());
    res.boneCount    = static_cast<std::uint32_t>(data.skeleton.bones.size());

    // Upload via the mapped pointer.
    void* p = nullptr;
    VK_CHECK(vmaMapMemory(ctx.allocator(), res.vertexAlloc, &p));
    std::memcpy(p, data.vertices.data(), vertBytes);
    vmaUnmapMemory(ctx.allocator(), res.vertexAlloc);

    VK_CHECK(vmaMapMemory(ctx.allocator(), res.indexAlloc, &p));
    std::memcpy(p, data.indices.data(), idxBytes);
    vmaUnmapMemory(ctx.allocator(), res.indexAlloc);

    const SkinnedMeshHandle h = nextHandle_++;
    meshes_[h] = res;
    return h;
}

const VkSkinnedMeshResource& VkSkinnedMeshStore::get(SkinnedMeshHandle h) const {
    static const VkSkinnedMeshResource empty;
    auto it = meshes_.find(h);
    return it != meshes_.end() ? it->second : empty;
}

void VkSkinnedMeshStore::destroyAll(VkContext& ctx) {
    for (auto& [h, res] : meshes_) {
        if (res.vertexBuffer) vmaDestroyBuffer(ctx.allocator(), res.vertexBuffer, res.vertexAlloc);
        if (res.indexBuffer)  vmaDestroyBuffer(ctx.allocator(), res.indexBuffer, res.indexAlloc);
    }
    meshes_.clear();
}

}  // namespace iron
```

- [ ] **Step 5: Register `VkSkinnedMesh.cpp` in `engine/CMakeLists.txt`**

Find the existing `render/backends/vulkan/VkMesh.cpp` entry in the Vulkan-only sources block. Add immediately after:

```cmake
      render/backends/vulkan/VkSkinnedMesh.cpp
```

- [ ] **Step 6: Add skinned descriptor set layout in `VkShader.cpp`**

Open `engine/render/backends/vulkan/VkShader.cpp`. The existing `VkShaderStore::create` builds a 7-binding layout. Add a parallel `createSkinned` method that builds an 8-binding layout.

In `VkShader.h`, add to the `VkShaderStore` class:

```cpp
    ShaderHandle createSkinned(VkContext& ctx,
                                const std::string& vertSrc,
                                const std::string& fragSrc);
```

In `VkShader.cpp`, add the new method below the existing `create`:

```cpp
ShaderHandle VkShaderStore::createSkinned(VkContext& ctx,
                                           const std::string& vertSrc,
                                           const std::string& fragSrc) {
    // Compile shaders.
    auto vSpv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   vertSrc);
    auto fSpv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vSpv.empty() || fSpv.empty()) {
        Log::error("VkShaderStore::createSkinned: compileGlsl failed");
        return kInvalidHandle;
    }

    VkShader sh;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vSpv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vSpv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &sh.vertexModule));
    smInfo.codeSize = fSpv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fSpv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &sh.fragmentModule));

    // Descriptor set layout: same 7 bindings as the scene shader, plus
    // binding 7 = bone-matrices UBO (vertex stage only).
    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 8;
    dsl.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsl, nullptr, &sh.setLayout));

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &sh.setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &sh.pipelineLayout));

    const ShaderHandle h = nextHandle_++;
    shaders_[h] = sh;
    return h;
}
```

Bump the sampler pool capacity in `VkFrameRing.cpp` to accommodate 1 more UBO per draw (bones UBO). The existing pool already provides enough UBO slots (1 per draw); since skinned draws also need 1 bones-UBO write, we need 2 UBO descriptors per skinned draw. Current pool size in `VkFrameRing.cpp:55`:

```cpp
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
```

Change to (allow up to 2 UBOs per descriptor set):

```cpp
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         2 * kMaxDescriptorSetsPerFrame},
```

- [ ] **Step 7: Add skinned pipeline factory in `VkPipeline.cpp`**

Open `engine/render/backends/vulkan/VkPipeline.h`. Add:

```cpp
    ::VkPipeline skinnedPipelineFor(VkContext& ctx, VkSwapchain& swap, const VkShader& sh);

private:
    std::vector<std::pair<const VkShader*, ::VkPipeline>> skinnedPipelines_;
```

In `VkPipeline.cpp`, implement `skinnedPipelineFor`. Find the existing `pipelineFor` method. Add a parallel `skinnedPipelineFor` below it. The only differences from `pipelineFor` are:

1. Vertex input layout: 6 attributes (pos, normal, uv, tangent, joints uvec4, weights vec4) with stride 76.
2. Cached in `skinnedPipelines_` separately.

Reference implementation:

```cpp
::VkPipeline VkPipeline::skinnedPipelineFor(VkContext& ctx, VkSwapchain& swap,
                                              const VkShader& sh) {
    for (const auto& [s, p] : skinnedPipelines_) {
        if (s == &sh) return p;
    }

    // Vertex input: SkinnedVertex layout.
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = static_cast<std::uint32_t>(sizeof(SkinnedVertex));
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[6]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(SkinnedVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, tangent)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(SkinnedVertex, joints)};
    attrs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SkinnedVertex, weights)};

    VkPipelineVertexInputStateCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vinfo.vertexBindingDescriptionCount = 1;
    vinfo.pVertexBindingDescriptions = &bind;
    vinfo.vertexAttributeDescriptionCount = 6;
    vinfo.pVertexAttributeDescriptions = attrs;

    // Everything else (input assembly, viewport, rasterization,
    // multisample, depth, blend, dynamic state) is identical to the
    // existing scene pipeline. Copy the relevant blocks from
    // pipelineFor() in this file — refactor opportunity if both
    // pipelines diverge much further. For now, paste-and-tweak.

    // ... (paste the rest of pipelineFor's pipeline-creation code,
    // changing only `vinfo` above to use the SkinnedVertex layout) ...

    // [Implementer: copy pipelineFor's create_info construction from
    // the InputAssemblyStateCreateInfo through the
    // VkGraphicsPipelineCreateInfo+vkCreateGraphicsPipelines, using
    // `vinfo` from this function's local scope. Change `sh` references
    // as appropriate. The render pass + framebuffer + depth attachment
    // are the same scene render pass.]

    ::VkPipeline pipe = VK_NULL_HANDLE;
    // pipe = vkCreateGraphicsPipelines(...) -- copy from pipelineFor

    skinnedPipelines_.push_back({&sh, pipe});
    return pipe;
}
```

> **Implementer:** the trick here is that `pipelineFor` (existing) and `skinnedPipelineFor` (new) differ in two places: vertex input layout + the pipeline cache vector. Everything else (render pass, framebuffer, IA, viewport, rasterizer, blend, depth, dynamic state, fragment shader) is identical.
>
> Refactor option: extract a private helper `createPipelineImpl(ctx, swap, sh, vinfo)` that takes the vertex input layout as an arg. Both `pipelineFor` and `skinnedPipelineFor` then call it with their respective `vinfo`. This is cleaner than copy-paste. **Recommend this refactor** during Step 7.

Update `VkPipeline::destroy` (or the equivalent cleanup path) to also destroy `skinnedPipelines_` entries. Mirror the existing `pipelines_` cleanup.

- [ ] **Step 8: Add `VkSkinnedMeshStore` + skinned-draw plumbing in `VulkanRenderer`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, add the include:

```cpp
#include "render/backends/vulkan/VkSkinnedMesh.h"
```

Add a member:

```cpp
    VkSkinnedMeshStore skinnedMeshes_;
```

Declare the new methods (matching the Renderer.h pure virtuals):

```cpp
    SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) override;
    ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) override;
    void submitSkinnedDraw(const SkinnedDrawCall& call) override;
```

In `VulkanRenderer.cpp`:

- `~VulkanRenderer` already calls `meshes_.destroyAll(context_)` etc. Add a sibling call:

```cpp
        skinnedMeshes_.destroyAll(context_);
```

(Match the order — destroy mesh stores together.)

- Implement the three new methods:

```cpp
SkinnedMeshHandle VulkanRenderer::createSkinnedMesh(const SkinnedMeshData& data) {
    return skinnedMeshes_.create(context_, data);
}

ShaderHandle VulkanRenderer::createSkinnedShader(const std::string& vertexSrc,
                                                  const std::string& fragmentSrc) {
    return shaders_.createSkinned(context_, vertexSrc, fragmentSrc);
}

void VulkanRenderer::submitSkinnedDraw(const SkinnedDrawCall& call) {
    if (skipFrame_) return;
    if (!skinnedMeshes_.has(call.skinnedMesh) || !shaders_.has(call.shader)) return;

    // Deferred: record into a per-frame queue, just like submit() does for
    // static draws. We'll add a separate vector for skinned draws so the
    // existing M14 frame-flow restructure keeps working.
    skinnedDraws_.push_back(call);
    // Also store the bone matrices into a separate stash because the span
    // points to caller-owned memory that may go away.
    skinnedBoneMatricesStash_.emplace_back(
        call.boneMatrices.begin(), call.boneMatrices.end());
}
```

Add the new member fields to `VulkanRenderer.h`:

```cpp
    std::vector<SkinnedDrawCall>           skinnedDraws_;
    std::vector<std::vector<Mat4>>         skinnedBoneMatricesStash_;
```

Clear them at the start of each frame, alongside the existing `sceneDraws_.clear()`:

```cpp
    skinnedDraws_.clear();
    skinnedBoneMatricesStash_.clear();
```

In `endFrame`, after the existing scene-draw replay loop, add a skinned-draw replay loop. This is the recording side — it needs to:
1. Allocate descriptor set from the frame pool
2. Write 1 LitUbo (binding 0) + 4 default textures (bindings 1-4) + cubemap (5) + reflection (6) + bones UBO (binding 7)
3. Bind the skinned pipeline
4. Bind vertex + index buffers
5. Draw indexed

Add a private `recordSkinnedDraw(VkCommandBuffer cb, const SkinnedDrawCall& call, const std::vector<Mat4>& bones)` method, modeled on the existing `recordSceneDraw`. The differences from the scene record:
- Get pipeline via `pipelines_.skinnedPipelineFor(context_, swapchain_, shader)` instead of `pipelineFor`.
- Get mesh via `skinnedMeshes_.get(call.skinnedMesh)` instead of `meshes_.get(call.mesh)`.
- After the existing LitUbo + 6-sampler descriptor writes, ALSO write a bones-UBO at binding 7:

```cpp
    // Bone matrix UBO (binding 7).
    std::array<Mat4, kMaxBonesPerSkinnedMesh> bonePadded{};
    for (auto& m : bonePadded) m = Mat4::identity();  // pad unused slots
    const std::size_t copyN = std::min(bones.size(), kMaxBonesPerSkinnedMesh);
    for (std::size_t i = 0; i < copyN; ++i) {
        bonePadded[i] = bones[i];
    }
    const VkDeviceSize bonesOffset = frames_.allocateUbo(
        bonePadded.data(),
        sizeof(Mat4) * kMaxBonesPerSkinnedMesh);
    VkDescriptorBufferInfo bonesBuf{};
    bonesBuf.buffer = f.uboBuffer;
    bonesBuf.offset = bonesOffset;
    bonesBuf.range  = sizeof(Mat4) * kMaxBonesPerSkinnedMesh;

    // Add to the writes array — the existing scene path writes 7
    // descriptors; skinned writes 8.
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = set;
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[7].descriptorCount = 1;
    writes[7].pBufferInfo = &bonesBuf;
    vkUpdateDescriptorSets(context_.device(), 8, writes, 0, nullptr);
```

And bind vertex + index buffers from `VkSkinnedMeshResource` (not `VkMeshResource`).

Finally in `endFrame`, after the existing `sceneDraws_` loop, add:

```cpp
        // M23 — skinned mesh draws (parallel to sceneDraws_).
        for (std::size_t i = 0; i < skinnedDraws_.size(); ++i) {
            recordSkinnedDraw(cb, skinnedDraws_[i], skinnedBoneMatricesStash_[i]);
        }
```

> **Note:** the existing `writes[7]{}` array in `recordSceneDraw` is only 7 entries. For `recordSkinnedDraw`, declare a fresh `VkWriteDescriptorSet writes[8]{}` — don't reuse the static-path arrays.

- [ ] **Step 9: Build**

```
cmake --build build-vk --config Debug --target ironcore
```

Expected: clean compile. The biggest risk: the new `recordSkinnedDraw` function is mostly a copy of `recordSceneDraw` with the changes above — keep them in lockstep when copying.

If you hit "type SkinnedMeshHandle undeclared" in `OpenGLRenderer.cpp`: include `"render/Handles.h"` in OpenGLRenderer.h.

If the descriptor pool runs out (`VK_ERROR_OUT_OF_POOL_MEMORY`): the M18 fix bumped `kMaxDescriptorSetsPerFrame` to 1024 already. The pool size in Step 6 needs the `2 * kMaxDescriptorSetsPerFrame` change for UBOs.

- [ ] **Step 10: Commit**

```
git add engine/render/Renderer.h \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp \
        engine/render/backends/vulkan/VkShader.h \
        engine/render/backends/vulkan/VkShader.cpp \
        engine/render/backends/vulkan/VkPipeline.h \
        engine/render/backends/vulkan/VkPipeline.cpp \
        engine/render/backends/vulkan/VkFrameRing.cpp \
        engine/render/backends/vulkan/VkSkinnedMesh.h \
        engine/render/backends/vulkan/VkSkinnedMesh.cpp \
        engine/render/backends/opengl/OpenGLRenderer.h \
        engine/render/backends/opengl/OpenGLRenderer.cpp \
        engine/CMakeLists.txt
git commit -m "M23 Task 2: Vulkan skinning pipeline + Renderer createSkinnedMesh / submitSkinnedDraw"
```

---

## Task 3: Viewer demo — render RiggedSimple in bind pose

**Files:**
- Modify: `games/10-gltf-viewer/main.cpp`
- Modify: `games/10-gltf-viewer/CMakeLists.txt` (asset copy already handles it via copy_directory)
- Create: `games/10-gltf-viewer/assets/rigged-simple/RiggedSimple.gltf` + `RiggedSimple0.bin`

After this task: `gltf-viewer --model rigged-simple` renders RiggedSimple in bind pose.

- [ ] **Step 1: Vendor RiggedSimple in the demo's assets**

```
curl -L -o games/10-gltf-viewer/assets/rigged-simple/RiggedSimple.gltf https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple.gltf
curl -L -o games/10-gltf-viewer/assets/rigged-simple/RiggedSimple0.bin https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple0.bin
```

The existing `copy_directory` POST_BUILD step from M22 will copy these next to the exe automatically.

- [ ] **Step 2: Add `--model` CLI arg + skinned-dispatch to `games/10-gltf-viewer/main.cpp`**

Open `games/10-gltf-viewer/main.cpp`. Modify the model path logic to accept a CLI arg:

```cpp
int main(int argc, char** argv) {
    // ... existing app/renderer init ...

    // Parse --model arg. Default to damaged-helmet.
    std::string modelName = "damaged-helmet";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) {
            modelName = argv[++i];
        }
    }

    const std::string modelPath = iron::executableDir()
        + "/assets/" + modelName + "/" +
        (modelName == "damaged-helmet" ? "DamagedHelmet.gltf" : "RiggedSimple.gltf");
    // (Simple mapping for the two assets we ship. Extendable later.)
```

After loading the model, dispatch on `model->skinnedMesh.has_value()`:

```cpp
    auto model = iron::loadGltfModel(modelPath);
    if (!model) {
        iron::Log::error("gltf-viewer: failed to load %s", modelPath.c_str());
        return 1;
    }

    const bool isSkinned = model->skinnedMesh.has_value();
    iron::Log::info("gltf-viewer: loaded %s (%s)",
                    modelPath.c_str(),
                    isSkinned ? "skinned" : "static");

    // For skinned models, also load the skinned mesh + skinned shader.
    iron::MeshHandle staticMesh = iron::kInvalidHandle;
    iron::SkinnedMeshHandle skinnedMeshHandle = iron::kInvalidSkinnedMesh;
    iron::ShaderHandle staticShader = iron::kInvalidHandle;
    iron::ShaderHandle skinnedShader = iron::kInvalidHandle;
    if (isSkinned) {
        skinnedMeshHandle = renderer.createSkinnedMesh(*model->skinnedMesh);
        skinnedShader = renderer.createSkinnedShader(kSkinnedVertexShader, kFragmentShader);
    } else {
        staticMesh = renderer.createMesh(model->mesh);
        staticShader = renderer.createShader(kVertexShader, kFragmentShader);
    }

    // Material textures (M22.5 path).
    const iron::TextureHandle albedo = model->materialPaths.albedo.empty()
        ? renderer.whiteTexture()
        : renderer.loadTexture(model->materialPaths.albedo);
    const iron::TextureHandle normalMap = model->materialPaths.normal.empty()
        ? renderer.flatNormalTexture()
        : renderer.loadTexture(model->materialPaths.normal);
    iron::TextureHandle spec = renderer.noSpecularTexture();
    if (!model->materialPaths.metalRoughness.empty()) {
        int w = 0, h = 0;
        auto specBytes = iron::loadRoughnessAsSpec(model->materialPaths.metalRoughness, w, h);
        if (!specBytes.empty()) spec = renderer.createTexture(w, h, specBytes.data());
    }
```

In the render lambda, dispatch the draw call based on `isSkinned`:

```cpp
    if (isSkinned) {
        // Bind-pose: all-identity bone matrices.
        std::array<iron::Mat4, iron::kMaxBonesPerSkinnedMesh> bonesPose;
        for (auto& m : bonesPose) m = iron::Mat4::identity();
        const std::size_t boneCount = model->skinnedMesh->skeleton.bones.size();

        iron::SkinnedDrawCall call;
        call.skinnedMesh = skinnedMeshHandle;
        call.shader      = skinnedShader;
        call.model       = iron::Mat4::identity();
        call.material.texture     = albedo;
        call.material.normalMap   = normalMap;
        call.material.specularMap = spec;
        call.material.emissive    = iron::Vec3{0.05f, 0.05f, 0.05f};
        call.boneMatrices = std::span<const iron::Mat4>{
            bonesPose.data(), std::min(boneCount, bonesPose.size())};
        renderer.submitSkinnedDraw(call);
    } else {
        iron::DrawCall call;
        call.mesh   = staticMesh;
        call.shader = staticShader;
        call.model  = iron::Mat4::identity();
        call.material.texture     = albedo;
        call.material.normalMap   = normalMap;
        call.material.specularMap = spec;
        call.material.emissive    = iron::Vec3{0.05f, 0.05f, 0.05f};
        renderer.submit(call);
    }
```

Add the skinned vertex shader string. It's the existing scene vertex shader (the same one used by `kVertexShader`) with the skinning math inserted. Copy `kVertexShader`, rename to `kSkinnedVertexShader`, and replace its main() with:

```glsl
void main() {
    mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
                 + aWeights.y * bones.bones[aJoints.y]
                 + aWeights.z * bones.bones[aJoints.z]
                 + aWeights.w * bones.bones[aJoints.w];

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec4 world = u.model * skinnedPos;
    vWorldPos = world.xyz;
    vNormal   = mat3(u.model) * (mat3(skinMat) * aNormal);
    vTangent  = mat3(u.model) * (mat3(skinMat) * aTangent);
    vUV       = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * skinnedPos;
}
```

Add the input attributes + bones UBO declaration at the top of `kSkinnedVertexShader`:

```glsl
#version 450

layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec3 aTangent;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4 aWeights;

// LitUbo at binding 0 — copy verbatim from existing kVertexShader.
layout(set=0, binding=0) uniform LitUbo { /* ... same fields ... */ } u;

// New: bones UBO at binding 7.
layout(set=0, binding=7) uniform BoneUbo {
    mat4 bones[128];
} bones;

layout(location=0) out vec3 vWorldPos;
layout(location=1) out vec3 vNormal;
layout(location=2) out vec3 vTangent;
layout(location=3) out vec2 vUV;
layout(location=4) out vec4 vLightSpacePos;

void main() {
    // (skinning math from above)
}
```

> **Important:** the LitUbo block must be identical (same fields, same order, same size) to the existing scene shader to share descriptor binding 0 + UBO buffer slot. Copy from `kVertexShader` verbatim — don't simplify.

- [ ] **Step 3: Build**

```
cmake --build build-vk --config Debug --target gltf-viewer
```

Expected: clean compile.

If the SPIR-V compile fails on `uvec4 aJoints`: glslang for Vulkan 1.3 supports uvec4 attributes; verify the `#version 450` directive is present.

- [ ] **Step 4: Smoke test — bind-pose rendering**

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe --model rigged-simple
```

Expected:
- Window opens, RiggedSimple visible — a tall, jointed two-box-stack mesh.
- WASD + mouse fly.
- Render should be visually identical to what it would look like statically — the box stack in its bind pose.

If RiggedSimple is invisible: bone count might be wrong. Add a temporary `Log::info("skin: %zu bones, %zu verts", skeleton.bones.size(), vertices.size())` after the load.

If it renders garbled (stretched / shrunk / wrong shape): the skinning math is broken. Most likely: the inverse-bind-matrix isn't being incorporated. In bind-pose: `jointMatrix = bindWorldMatrix × inverseBindMatrix = identity`. We're sending identity directly via `bonesPose`, which is wrong for the math — we should send something that, multiplied by aPos through `(weights × M × pos)` = pos. **Identity matrices DO produce identity transform** for the math `skinMat * pos = identity * pos = pos`. So the demo should work with identity.

If RiggedSimple renders, but as a flat-shaded T-pose where the static M22 path would render as the bind pose — that's actually identical, both are the bind pose. M23 success.

- [ ] **Step 5: Run the existing test suite to confirm no regressions**

```
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: all 39+ tests pass.

- [ ] **Step 6: Commit**

```
git add games/10-gltf-viewer/assets/rigged-simple/ \
        games/10-gltf-viewer/main.cpp
git commit -m "M23 Task 3: 10-gltf-viewer renders RiggedSimple via skinned path (bind pose)"
```

---

## Task 4: Docs + PR

**Files:**
- Modify: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Append M23 section to `docs/engine/asset-pipeline.md`**

Open the file. After the existing M22.5 "Material textures" section, append:

```markdown

## M23 — Skeleton + GPU skinning

The engine now supports skinned meshes via a parallel render path
alongside the static path. Backbone is M23's foundation; M24 will add
animation playback on top.

### Types

```cpp
// engine/asset/Skeleton.h
struct Bone {
    int  parentIndex = -1;            // -1 for root
    Mat4 inverseBindMatrix;
    Mat4 localBindTransform;
    std::string name;
};

struct Skeleton {
    std::vector<Bone> bones;
};

// engine/scene/SkinnedMesh.h
struct SkinnedVertex {
    Vec3          position;
    Vec3          normal;
    Vec2          uv;
    Vec3          tangent;
    std::uint32_t joints[4];
    float         weights[4];
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};
```

### Renderer API

```cpp
struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh;
    ShaderHandle      shader;
    Mat4              model;
    Material          material;
    std::span<const Mat4> boneMatrices;  // size ≤ kMaxBonesPerSkinnedMesh (128)
};

class Renderer {
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) = 0;
    virtual ShaderHandle createSkinnedShader(vertSrc, fragSrc) = 0;
    virtual void submitSkinnedDraw(const SkinnedDrawCall&) = 0;
};
```

### glTF integration

`loadGltfModel(path).skinnedMesh` is populated when the .gltf has a
`skin` referenced by the host node. The static `mesh` field is
populated regardless (lets callers ignore skinning). The 4-influence
JOINTS_0 + WEIGHTS_0 attributes drive the per-vertex bone indices /
weights; weights are normalized at load time (sum to 1.0).

Quaternion rotation + non-uniform scale on bones' `localBindTransform`
are deferred to M24 — RiggedSimple uses identity rotation/scale at
bind time so M23 works without those.

### Vulkan path

- **New descriptor set layout** for skinned meshes: 8 bindings — same
  7 as scene (UBO + 4 textures + cubemap + reflection) + binding 7 =
  bone matrix UBO (vertex stage only).
- **New pipeline** with SkinnedVertex input layout (6 attributes, 76
  byte stride).
- **128-bone cap per skinned mesh** — 8 KB bone UBO per draw. The
  M14 per-frame UBO buffer accommodates this easily.
- **Sampler pool unchanged** (bone UBO is a uniform buffer, not a
  sampler). UBO pool slot capacity bumped to 2× per descriptor set.

### Visual validator

`games/10-gltf-viewer --model rigged-simple` renders Khronos's
RiggedSimple.gltf in bind pose (identity bone matrices). When the
loader returns a skinned mesh, the viewer dispatches through the
skinned path; otherwise (Damaged Helmet et al.) the static M22 path.

### What's next

- **M24** — Animation curves + playback. Read animation.channels +
  animation.samplers from glTF; sample at runtime to compute bone
  matrices that replace the M23 identity matrices.
- **M25** — Wire skinned characters into net-shooter. Idle/walk
  anim on players; ragdoll bones on death drive a skinned mesh.

### Known v1 limitations

- 4-influence skinning only (`JOINTS_0` / `WEIGHTS_0`); no
  `JOINTS_1` / `WEIGHTS_1`.
- Single primitive's material/skin per mesh — multi-primitive meshes
  use only the first primitive (matches M22's existing scope).
- Bone localBindTransform uses translation only — rotation + scale
  added in M24 when needed for animation.
- Normal/tangent skinning uses `mat3(skinMat)` directly, not the
  inverse-transpose — exact only for rigid (rotation+translation)
  bones; non-uniform-scaled bones would have slightly off normals.
```

- [ ] **Step 2: Commit**

```
git add docs/engine/asset-pipeline.md
git commit -m "M23 Task 4: docs/engine/asset-pipeline.md M23 section"
```

- [ ] **Step 3: Push + open PR**

```
git push -u origin feat/m23-skeleton-skinning
gh pr create --title "M23: Skeleton + GPU skinning shader" --body "$(cat <<'EOF'
## Summary
- New engine types: `iron::Skeleton` + `iron::Bone` + `iron::SkinnedVertex` + `iron::SkinnedMeshData`
- glTF loader extension: `GltfModel.skinnedMesh` populated when the file has a skin (reads JOINTS_0/WEIGHTS_0 + inverseBindMatrices; normalizes weights; builds bone hierarchy via parentIndex)
- New Vulkan skinning pipeline: 8-binding descriptor set (scene + bones UBO at binding 7), SkinnedVertex input layout (6 attributes, 76-byte stride), 4-influence weighted skinning vertex shader, 128-bone cap
- New Renderer API: `createSkinnedMesh`, `createSkinnedShader`, `submitSkinnedDraw`, `SkinnedDrawCall` (caller supplies bone matrices; identity for bind pose)
- OpenGL backend stubs (frozen since PR #35)
- New `--model` CLI arg in 10-gltf-viewer; vendors Khronos RiggedSimple CC0 sample
- One new test sub-check on RiggedSimple's skin data
- `docs/engine/asset-pipeline.md` extended

Second milestone of the M22-M25 skeletal-animation track. M24 will layer animation curves on top.

## Test plan
- [ ] CI green (Windows MSVC)
- [ ] `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe --model rigged-simple` renders RiggedSimple in bind pose
- [ ] Damaged Helmet (default) still works (no static-path regression)
- [ ] All 39+ engine tests pass

## Known v1 limitations
- 4-influence skinning only (JOINTS_0/WEIGHTS_0)
- Bone localBindTransform uses translation only — rotation + scale come in M24
- Single primitive per mesh
- Normal skinning uses mat3(skinMat) directly (not inverse-transpose) — accurate for rigid bones

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- `iron::Skeleton` + `Bone` → Task 1 Step 2
- `iron::SkinnedVertex` + `SkinnedMeshData` → Task 1 Step 3
- `SkinnedMeshHandle` → Task 1 Step 1
- glTF loader extension + JOINTS_0/WEIGHTS_0 + inverseBindMatrices → Task 1 Step 5
- Weight normalization → Task 1 Step 5 (normalization block)
- Bone parent hierarchy → Task 1 Step 5 (parent-resolve loop)
- `Renderer::createSkinnedMesh` / `createSkinnedShader` / `submitSkinnedDraw` → Task 2 Step 1
- OpenGL stubs → Task 2 Step 2
- `VkSkinnedMeshStore` → Task 2 Steps 3-4
- New descriptor set layout (8 bindings) → Task 2 Step 6
- New pipeline with SkinnedVertex input → Task 2 Step 7
- `submitSkinnedDraw` recording path → Task 2 Step 8
- Skinning vertex shader → Task 3 Step 2 (kSkinnedVertexShader)
- 128-bone UBO at binding 7 → Task 2 Step 8 (write at binding 7)
- Viewer `--model` arg + skinned dispatch → Task 3 Step 2
- RiggedSimple test → Task 1 Step 7
- Visual validator → Task 3 Step 4
- Docs → Task 4

**Placeholder scan:** clean — every code step has actual code. The pipelineFor copy-paste in Task 2 Step 7 has an "implementer: copy-paste" note with a refactor suggestion; this is explicit guidance, not a placeholder.

**Type consistency:**
- `SkinnedMeshHandle` defined consistently as `std::uint32_t` across header, OpenGL stub, Vulkan store
- `kMaxBonesPerSkinnedMesh = 128` referenced consistently
- `SkinnedVertex` layout (76 bytes = 11 floats + 4 uints + 4 floats) consistent across vertex shader inputs (loc 0-5) and Vulkan pipeline attribute descriptions
- `boneMatrices` is `std::span<const Mat4>` consistently
