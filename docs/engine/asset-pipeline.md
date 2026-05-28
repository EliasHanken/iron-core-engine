# Asset pipeline

This is the home for engine-side asset import. Currently covers glTF
mesh loading (M22+); future tracks add texture-import polish, audio
loading, level format, etc.

## glTF loader (`iron::loadGltfModel`)

`engine/asset/GltfLoader.{h,cpp}` ships a two-function API
(`loadGltfMesh` is a thin wrapper for callers that don't need
material paths — see the M22.5 section below):

```cpp
std::optional<GltfModel> loadGltfModel(const std::string& path);
std::optional<MeshData>  loadGltfMesh (const std::string& path);
```

Backed by [tinygltf](https://github.com/syoyo/tinygltf) via vcpkg.
Loads the first primitive of the first mesh of the first node of the
default scene from a `.gltf` (JSON + adjacent `.bin`) or `.glb` (single
binary) file. tinygltf is header-only; its implementation lives only in
`GltfLoader.cpp` (via `TINYGLTF_IMPLEMENTATION`). The header is added
to `ironcore`'s PRIVATE include paths so game code never sees tinygltf.

### Vertex attribute mapping

| glTF attribute  | Engine `Vertex`     | Required | Fallback         |
| --------------- | ------------------- | -------- | ---------------- |
| `POSITION`      | `position` (Vec3)   | yes      | hard-fail        |
| `NORMAL`        | `normal` (Vec3)     | yes      | hard-fail        |
| `TEXCOORD_0`    | `uv` (Vec2)         | no       | `(0, 0)`         |
| `TANGENT`       | `tangent` (Vec3)    | no       | `(1, 0, 0)`      |
| `indices`       | `indices` (u32)     | yes      | hard-fail        |

`TANGENT` is `vec4` in glTF (the `w` stores handedness sign); we drop
`w` and store the xyz. Index buffers come as `u8`, `u16`, or `u32` from
glTF; the loader promotes everything to `u32` at load time so the
engine's mesh interface is always `std::uint32_t`.

### What's NOT loaded in M22

- **PBR shading.** Textures are bound through the existing Blinn-Phong
  lit shader. Metallic-roughness conversion to "spec map" is
  approximate (G channel only, metallic discarded). Proper PBR
  (metallic-roughness BRDF + image-based lighting + tone-mapping) is a
  future track.
- **Skeletons, joints, skinning data** — M23.
- **Animations** — M24.
- **Multi-primitive meshes, multi-mesh scenes, scene-graph traversal
  beyond `scene[default].nodes[i]` with one level of child recursion**
  — extended in later milestones if needed.
- **Node-local transforms.** The loader returns positions in the
  mesh's local space, ignoring any node transform matrix. Most simple
  models (Damaged Helmet, Box) are already in a sensible local space,
  so this works for v1.
- **MikkTSpace tangent synthesis** for models that don't ship tangents
  — fallback `(1, 0, 0)` is used; visual artifact only when normal
  maps are also in use.
- **glTF 2.0 extensions** (`KHR_draco_compression`,
  `KHR_texture_basisu`, `KHR_lights_punctual`, etc.) — not supported.

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
texture set applied (under the existing Blinn-Phong shader — proper
PBR is a future track).

### Visual validator: `games/10-gltf-viewer`

Loads the Khronos "Damaged Helmet" CC0 sample. Vulkan-only; free-fly
camera; HUD shows vert/tri counts. Run with:

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe
```

Helmet renders with its full material texture set (base color, normal
map, metal-roughness → spec) under the existing Blinn-Phong shader —
proves both the loader and the M22.5 texture pipeline work end-to-end.

### Test assets

Three small Khronos CC0 samples are vendored under
`tests/assets/gltf/`:
- `Box.gltf` + `Box0.bin` — 24-vertex unit cube; positive test for the
  basic load path.
- `BoxTextured.gltf` + `BoxTextured0.bin` + `CesiumLogoFlat.png` —
  same geometry with non-zero UVs; positive test for the optional
  TEXCOORD_0 attribute.
- `Triangle.gltf` + `Triangle.bin` — minimal 3-vertex sample WITHOUT
  NORMAL. Used as a **negative test**: confirms the loader returns
  `nullopt` when required attributes are missing.

## M23 — Skeleton + GPU skinning

The engine now supports skinned meshes via a parallel render path
alongside the static path. M23 ships the foundation; M24 will add
animation playback on top.

### Engine types

```cpp
// engine/asset/Skeleton.h
struct Bone {
    int  parentIndex = -1;            // -1 for root
    Mat4 inverseBindMatrix;
    Mat4 localBindTransform;          // translation only in v1; rotation/scale → M24
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
    std::uint32_t joints[4];   // bone indices into Skeleton::bones
    float         weights[4];  // normalized: sum to 1.0
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex>  vertices;
    std::vector<std::uint32_t>  indices;
    Skeleton                    skeleton;
};
```

### Renderer API

```cpp
inline constexpr std::size_t kMaxBonesPerSkinnedMesh = 128;

struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh;
    ShaderHandle      shader;
    Mat4              model;
    Material          material;
    std::span<const Mat4> boneMatrices;  // size ≤ kMaxBonesPerSkinnedMesh
};

class Renderer {
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData&) = 0;
    virtual ShaderHandle createSkinnedShader(vertSrc, fragSrc) = 0;
    virtual void submitSkinnedDraw(const SkinnedDrawCall&) = 0;
};
```

`boneMatrices` is the array of joint matrices (`worldTransform × inverseBindMatrix`).
For **bind pose**, all identity (the visual M23 validator). M24's
`AnimationPlayer` will sample animation curves to compute these per frame.

OpenGL backend stubs the three methods (warn-once + return invalid handles)
— it's been frozen since PR #35 and isn't getting new feature parity.

### glTF integration

`loadGltfModel(path).skinnedMesh` is populated when the .gltf has a
`skin` referenced by the host node. The static `mesh` field is
populated regardless, so callers that don't care about skinning can
ignore `skinnedMesh` entirely.

The loader:
- Reads `JOINTS_0` (u8 or u16 → uint32) and `WEIGHTS_0` (vec4 float) per-vertex attributes.
- Reads `skin.inverseBindMatrices` (MAT4 float accessor).
- Builds the bone hierarchy by walking each joint node's children + matching against `skin.joints[]` to resolve `parentIndex`.
- Normalizes per-vertex weights to sum to 1.0 (defense for non-strict assets).
- Bone `localBindTransform` reads the node's matrix (column-major) OR its `translation` TRS — rotation + scale TRS are deferred to M24 (RiggedSimple uses identity rot/scale, so M23 works without them).
- Scene-walk is a depth-first traversal (RiggedSimple's mesh is 3 levels deep: `Z_UP → Armature → Cylinder`, deeper than the single-level walk M22 used).

### Vulkan path

- **New descriptor set layout** for skinned meshes: 8 bindings — same
  7 as scene (UBO + 4 textures + cubemap + reflection) + binding 7 =
  bone matrix UBO (vertex stage only). Built via
  `VkShaderStore::createSkinned`.
- **New pipeline** with SkinnedVertex input layout (6 attributes, 76
  byte stride). Cached separately in `VkPipeline::skinnedPipelineFor`.
  The static pipeline build was refactored into a shared helper to
  avoid drift.
- **128-bone cap per skinned mesh**. Bone UBO is 8 KB per draw.
- **UBO descriptor pool capacity bumped to 2×** `kMaxDescriptorSetsPerFrame`
  since each skinned draw uses 2 UBOs (scene + bones).
- **Recording**: `recordSkinnedDraw` mirrors `recordSceneDraw` — same
  LitUbo construction and 6 texture writes — adds an 8 KB bones-UBO
  allocation and writes binding 7. Replayed after the static-draw
  loop inside `endFrame`'s scene pass.
- **Bone matrices padded to 128 identity** on the host side before
  upload so the shader can index safely beyond the actual bone count.

### Skinning vertex shader

Standard 4-influence weighted skinning:

```glsl
mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
             + aWeights.y * bones.bones[aJoints.y]
             + aWeights.z * bones.bones[aJoints.z]
             + aWeights.w * bones.bones[aJoints.w];

vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
vec4 world      = u.model * skinnedPos;
vNormal  = mat3(u.model) * (mat3(skinMat) * aNormal);
gl_Position = u.mvp * skinnedPos;
```

Fragment shader is unchanged from the scene path — skinning is purely
a vertex stage concern.

### Visual validator: `games/10-gltf-viewer --model rigged-simple`

The viewer's `--model` CLI arg selects between vendored samples:
- `damaged-helmet` (default, from M22) — static path, M22.5 textures applied
- `rigged-simple` — skinned path, RiggedSimple.gltf in bind pose

Identity bone matrices produce identity transforms — the skinned mesh
renders at the same place a static render of the same vertices would.
Bind-pose validation: if the skinning math is correct, the result
matches what an equivalent static render would show.

### What's next

- **M24** — Animation curves + playback. Read `animation.channels` +
  `animation.samplers` from glTF; sample at runtime to compute bone
  matrices that replace the M23 identity matrices.
- **M25** — Wire skinned characters into net-shooter. Idle/walk
  anim on players; ragdoll bones on death drive a skinned mesh.

### Known v1 limitations

- 4-influence skinning only (`JOINTS_0` / `WEIGHTS_0`); no `JOINTS_1` /
  `WEIGHTS_1` for 8-influence skinning.
- Bone `localBindTransform` reads matrix OR translation-only TRS —
  rotation (quaternion) + non-uniform scale handling lands in M24.
- Single primitive per mesh (multi-primitive meshes deferred).
- Normal/tangent skinning uses `mat3(skinMat)` directly — exact for
  rigid (rotation+translation) bones; non-uniform-scaled bones would
  have slightly off normals (use inverse-transpose if needed).
- Scene-walk depth-first traversal picks the first mesh found —
  multi-mesh scenes would need explicit selection.
