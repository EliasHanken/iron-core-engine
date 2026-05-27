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

## What's next

This is the first milestone of a 4-milestone skeletal-animation +
glTF asset track:

- **M23** — Skeleton + GPU skinning shader. Extend the loader to read
  `skin` data; new `iron::Skeleton` + `iron::SkinnedMesh` types; new
  Vulkan skinning vertex shader. Render the same model with its
  skeleton in bind pose.
- **M24** — Animation curves + playback. Sample glTF animation curves
  at runtime to compute bone transforms.
- **M25** — Wire skinned characters into net-shooter. Idle/walk
  anims on players. Stretch: ragdoll bones drive the skinned mesh on
  death for "ragdoll using the character's mesh" effect.

**M22.5** (shipped) added the base-color / normal / metal-roughness
texture path loading; see section above.
