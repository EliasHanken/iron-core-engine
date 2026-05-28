# M23 — Skeleton + GPU Skinning Shader (Design Spec)

**Date:** 2026-05-27
**Milestone:** M23 (second of the skeletal-animation + glTF asset pipeline track)
**Status:** Design — awaiting implementation plan

## Goal

Bind-pose rendering of a skinned glTF model. After M23 lands the engine has `iron::Skeleton` + `iron::SkinnedMeshData` types, a Vulkan skinning vertex pipeline (4-influence weighted skinning with a per-draw bone-matrix UBO), and renderer methods `createSkinnedMesh` + `submitSkinnedDraw`. The viewer loads Khronos's `RiggedSimple.gltf` (CC0, ~3 KB), fills identity bone matrices, and renders the rigged box correctly. M24 then layers animation curves on top of this foundation.

## Direction context

M22 + M22.5 shipped static glTF mesh loading with material textures. The next visual leap is animated characters — needed for the M25 net-shooter wiring goal. M23 is the structural milestone of the skeletal track: it builds the engine plumbing for skinned meshes (vertex format, descriptor layout, pipeline, shader) without yet adding animation playback. The validator is bind-pose rendering, which exercises every piece of the new path except the time-varying bone-matrix computation that M24 will add.

## Non-Goals

- **Animation playback.** No keyframe interpolation, no time-varying bone transforms. M24's territory.
- **Skinned meshes in net-shooter.** Engine support only; net-shooter integration is M25.
- **Multiple skinned meshes per character** (e.g., separate head + body meshes sharing a skeleton). The loader handles a single primitive's skin; multi-mesh characters wait.
- **CPU skinning fallback.** GPU only. The engine is Vulkan-only post-PR-#35.
- **More than 4 bone influences per vertex.** glTF supports `JOINTS_1`/`WEIGHTS_1` for 8-influence skinning; v1 reads only `JOINTS_0`/`WEIGHTS_0` (almost universal in practice).
- **More than 128 bones per skinned mesh.** UBO-backed bone-matrix array sized for 128 mat4s (8 KB). Humanoid skeletons typically have 50-80 bones; well under cap.
- **PBR shading.** Skinning is purely a vertex transform — the fragment shader stays Blinn-Phong from earlier milestones.

## Architecture

### 1. Engine types — `engine/asset/Skeleton.h` + `engine/scene/SkinnedMesh.h`

```cpp
// engine/asset/Skeleton.h
namespace iron {

struct Bone {
    int  parentIndex = -1;            // -1 for root
    Mat4 inverseBindMatrix;            // glTF inverseBindMatrices[i]
    Mat4 localBindTransform;           // bone's local-space TRS at bind time
    std::string name;                  // diagnostic; not load-bearing
};

struct Skeleton {
    std::vector<Bone> bones;
};

}  // namespace iron
```

```cpp
// engine/scene/SkinnedMesh.h
#include "asset/Skeleton.h"

namespace iron {

// Skinned vertex format: existing geometry + 4 joints + 4 weights.
struct SkinnedVertex {
    Vec3      position;
    Vec3      normal;
    Vec2      uv;
    Vec3      tangent;
    std::uint32_t joints[4];   // bone indices into Skeleton::bones; packed as uvec4
    float         weights[4];  // per-influence weights; normalized (sum ≈ 1)
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};

}  // namespace iron
```

### 2. Renderer API additions (`engine/render/Renderer.h`)

```cpp
using SkinnedMeshHandle = std::uint32_t;
inline constexpr SkinnedMeshHandle kInvalidSkinnedMesh = 0;

struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh = kInvalidSkinnedMesh;
    ShaderHandle      shader      = kInvalidHandle;
    Mat4              model       = Mat4::identity();
    Material          material;
    // Joint matrices: world transform of each bone × inverseBindMatrix.
    // For bind pose, all identity. M24's AnimationPlayer overwrites
    // these per frame. Caller MUST ensure size ≤ kMaxBonesPerSkinnedMesh.
    std::span<const Mat4> boneMatrices;
};

class Renderer {
public:
    // ... existing methods ...
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) = 0;
    virtual void submitSkinnedDraw(const SkinnedDrawCall&) = 0;
};

inline constexpr std::size_t kMaxBonesPerSkinnedMesh = 128;
```

### 3. Vulkan path

**New per-frame UBO sub-allocation:** the per-draw `LitUbo` (928 bytes, from M17) is reused unchanged; a separate `BoneMatricesUbo` of `kMaxBonesPerSkinnedMesh × sizeof(Mat4)` = 8 KB is allocated per skinned draw call from the same per-frame UBO buffer.

**New descriptor set layout for skinned meshes**: 8 bindings — same 7 as the scene pass (UBO + diffuse + normal + spec + shadow + cubemap + reflection) plus binding 7 = bone-matrices UBO. Lives in a new shader-store entry alongside the existing scene shader.

**New `VkSkinnedMeshStore` (engine/render/backends/vulkan/VkSkinnedMesh.{h,cpp})**: parallel to `VkMeshStore`. Stores `SkinnedMeshHandle → { VkBuffer vertices, VkBuffer indices, std::uint32_t indexCount }`. Vertex stride is 76 bytes: 11 floats + 4 uints + 4 floats.

**New pipeline in `VkPipeline`**: vertex input has 6 attributes (pos, normal, uv, tangent, joints, weights). The pipeline factory caches one pipeline per skinned shader, similar to how the scene pipeline is cached.

**Vertex shader**:

```glsl
#version 450

layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec3 aTangent;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4 aWeights;

layout(set=0, binding=0) uniform LitUbo { /* M17 layout, unchanged */ } u;
layout(set=0, binding=7) uniform BoneUbo { mat4 bones[128]; } bones;

// outs match the existing scene shader for fragment compatibility
layout(location=0) out vec3 vWorldPos;
layout(location=1) out vec3 vNormal;
layout(location=2) out vec3 vTangent;
layout(location=3) out vec2 vUV;
layout(location=4) out vec4 vLightSpacePos;

void main() {
    // Weighted skinning: blend 4 bone matrices.
    mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
                 + aWeights.y * bones.bones[aJoints.y]
                 + aWeights.z * bones.bones[aJoints.z]
                 + aWeights.w * bones.bones[aJoints.w];

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec4 world      = u.model * skinnedPos;

    vWorldPos = world.xyz;
    // Normals transformed by the rotational 3x3 of the skin matrix.
    // For correctness with non-uniform scale, this should be the
    // inverse-transpose, but per-vertex skinned models are uniform so
    // mat3(skinMat) is accurate enough for v1.
    vNormal  = mat3(u.model) * (mat3(skinMat) * aNormal);
    vTangent = mat3(u.model) * (mat3(skinMat) * aTangent);
    vUV      = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * skinnedPos;
}
```

**Fragment shader**: identical to the existing scene-pass fragment shader (Blinn-Phong with the M17 binding layout). Skinning is purely a vertex stage concern.

### 4. glTF loader extension

`engine/asset/GltfLoader.h` — extend `GltfModel`:

```cpp
struct GltfModel {
    MeshData                       mesh;
    GltfMaterialPaths              materialPaths;
    std::optional<SkinnedMeshData> skinnedMesh;  // populated if glTF has a skin
};
```

The loader walks `prim.attributes` for `JOINTS_0` (vec4 of u8/u16) + `WEIGHTS_0` (vec4 of float). If the host node has `skin >= 0`, populates `skinnedMesh`:
- `skin.joints[]` → bone indices in the glTF nodes array
- For each bone: build the `Bone` struct via the node's local TRS (translation/rotation/scale) → `localBindTransform`; resolve parent index from the joint hierarchy; read inverseBindMatrices[i] from the skin's accessor
- Bake the per-vertex `JOINTS_0`/`WEIGHTS_0` into the engine's `SkinnedVertex` joints/weights

The static `mesh` field continues to be populated for back-compat — callers can ignore it if they only want skinning.

### 5. Visual validator — extend `games/10-gltf-viewer`

The viewer is extended to dispatch on what the loader returned:
- If `model.skinnedMesh` is set → call `renderer.createSkinnedMesh(*model.skinnedMesh)`, fill an array of `kMaxBonesPerSkinnedMesh` identity matrices, call `renderer.submitSkinnedDraw(call)`.
- Otherwise → existing static path (no behavior change for Damaged Helmet).

Vendor `RiggedSimple.gltf` under `games/10-gltf-viewer/assets/rigged-simple/` (~3 KB CC0 from Khronos). Add a CLI arg `--model rigged-simple` to switch which asset loads (default keeps Damaged Helmet).

### 6. Engine config

`engine/render/backends/vulkan/VkFrameRing.h`: bump per-frame UBO buffer if needed. Current size from M14 should already accommodate (per-draw scene UBO + per-skinned-draw bones UBO + bookkeeping). Verify; bump if hot.

## Tasks

Four subagent-friendly chunks:

1. **Engine types + glTF loader extension** — `engine/asset/Skeleton.h`, `engine/scene/SkinnedMesh.h`, `iron::loadGltfModel` populates `skinnedMesh` from skin data + JOINTS_0/WEIGHTS_0. New test: load `RiggedSimple.gltf`, verify joint count + bone parent hierarchy + inverseBindMatrix shape.

2. **Vulkan skinning pipeline + renderer API** — `VkSkinnedMeshStore`, new descriptor set layout with binding 7, new pipeline in `VkPipeline`, new skinning vert shader (frag reuses scene shader). `Renderer::createSkinnedMesh` + `submitSkinnedDraw` implementations in `VulkanRenderer`. Bone-matrix UBO sub-allocation per draw.

3. **Viewer demo: skinned-model path** — vendor `RiggedSimple.gltf`, add CLI arg, dispatch on `skinnedMesh.has_value()`, fill identity bone matrices, submit through skinned path. Visual validator: rigged box renders correctly in bind pose.

4. **Docs** — append M23 section to `docs/engine/asset-pipeline.md`.

## Tests

`tests/test_gltf_loader.cpp` gains one sub-test:

```cpp
// --- RiggedSimple.gltf has a skin with bones + joints/weights ---
{
    auto model = loadGltfModel(base + "/RiggedSimple.gltf");
    CHECK(model.has_value());
    CHECK(model->skinnedMesh.has_value());
    const auto& sm = *model->skinnedMesh;
    // RiggedSimple has a 2-bone skeleton (a root + a child).
    CHECK(sm.skeleton.bones.size() == 2);
    // Each vertex has 4 joint indices + weights summing to ~1.0.
    for (const auto& v : sm.vertices) {
        const float wsum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
        CHECK(wsum > 0.99f && wsum < 1.01f);
    }
    // Inverse-bind matrices array matches bone count.
    // (Verified by Skeleton invariant — every bone has one.)
}
```

Vendor `RiggedSimple.gltf` + `RiggedSimple.bin` under `tests/assets/gltf/`.

No new engine unit tests for the Vulkan path — that's GPU-bound; the demo is the validator.

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| RiggedSimple's bind pose isn't visually distinctive | True — it's just a box. Compare against the same box rendered statically (treat `mesh` field, not `skinnedMesh`). If pixel-identical, skinning math is correct. |
| JOINTS_0 accessor component type varies (u8 / u16) | tinygltf normalizes via `tinygltf::ComponentTypeByteSize` — read with appropriate width, promote to `uint32_t`. |
| WEIGHTS_0 might not be normalized in some assets | Normalize at load time: divide each weight by the sum. RiggedSimple is well-formed; this is defense for future assets. |
| Bone-matrix UBO at 8 KB × N skinned draws bloats per-frame UBO buffer | M14's per-frame UBO is 256 KB. 8 KB per draw × ~32 max skinned draws = 256 KB. Bump if it ever overflows — for v1 (one skinned model in the viewer), trivially fits. |
| New descriptor set layout requires another sampler-pool slot bump | Skinned path adds 1 UBO binding, NOT a sampler. No sampler pool change needed. |
| Non-uniform scale on skinned meshes breaks normal transform | Acknowledged — mat3(skinMat) is approximate. Documented; not an issue for v1 assets. |
| Multiple inheritance of vertex format complicates pipeline cache | Static + skinned are fully separate paths. No shared cache key. Clean. |

## Verification

- **CI green** — `test_gltf_loader` passes the new sub-test; existing tests still pass.
- **Solo viewer**: `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe --model rigged-simple` renders RiggedSimple's two-box rigged geometry in bind pose. WASD + mouse fly.
- **A/B test**: when the loader returns BOTH a static `mesh` AND a `skinnedMesh` for the same asset, rendering each separately should look pixel-identical (skinning with identity bones is a no-op transform).
- **No regression** on Damaged Helmet (default Damaged Helmet still loads through static path).

## Follow-ups (NOT in M23)

- **M24** — Animation curves + playback. `iron::Animation` + `iron::AnimationPlayer` sample curves at runtime to compute bone matrices.
- **M25** — Wire skinned characters into net-shooter (idle/walk anims; ragdoll-on-death drives mesh bones).
- **MikkTSpace tangent synthesis** for skinned meshes without baked tangents.
- **8-influence skinning** (JOINTS_1/WEIGHTS_1) — rare in modern assets.
- **Bone-matrix SSBO instead of UBO** — only if max-bones cap becomes a constraint.
- **Per-bone debug-line gizmos** — render the skeleton as colored lines for animation debugging.
