# M22 Static glTF Mesh Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `tinygltf` via vcpkg, ship an `iron::loadGltfMesh(path)` engine API that maps a glTF file's first primitive into `iron::MeshData`, and validate with a new `games/10-gltf-viewer` Vulkan demo loading the Khronos "Damaged Helmet" CC0 sample under the existing lit shader.

**Architecture:** New `engine/asset/GltfLoader.{h,cpp}` — pimpl-free wrapper that includes `<tinygltf.h>` only in the .cpp. Loader walks `model.scenes[default].nodes[0].mesh.primitives[0]`, reads POSITION/NORMAL (required) + TEXCOORD_0/TANGENT (optional with defaults) accessors, promotes u16 indices to u32, returns `std::optional<MeshData>`. tinygltf linked PRIVATE to ironcore so no header bleed to game code. Demo vendors Damaged Helmet (~200KB CC0) next to the exe via `copy_directory` POST_BUILD, mirrors the asset-loading pattern from `games/07-net-shooter`.

**Tech Stack:** C++23, tinygltf via vcpkg, Vulkan 1.3, CMake, MSVC.

---

## File Structure

### New files
- `engine/asset/GltfLoader.h` — public API (`loadGltfMesh`)
- `engine/asset/GltfLoader.cpp` — tinygltf-touching impl
- `tests/test_gltf_loader.cpp` — 4 sub-tests
- `tests/assets/gltf/Box.gltf`, `tests/assets/gltf/Box0.bin` (Khronos CC0)
- `tests/assets/gltf/BoxTextured.gltf`, `tests/assets/gltf/Box0.bin` (shared with above) + the texture files
- `tests/assets/gltf/Triangle.gltf` (embedded buffer — single file)
- `games/10-gltf-viewer/CMakeLists.txt`
- `games/10-gltf-viewer/main.cpp`
- `games/10-gltf-viewer/assets/damaged-helmet/*` (Khronos CC0 — .gltf + .bin + textures, ~200KB total)
- `docs/engine/asset-pipeline.md` — new docs file for the M22-M25 track

### Modified files
- `vcpkg.json` — add `"tinygltf"`
- `CMakeLists.txt` (top-level) — `find_package(tinygltf CONFIG REQUIRED)` after the existing Jolt find_package; `add_subdirectory(games/10-gltf-viewer)`
- `engine/CMakeLists.txt` — register `asset/GltfLoader.cpp`; link `tinygltf::tinygltf` PRIVATE
- `tests/CMakeLists.txt` — register `test_gltf_loader` with `IRON_REPO_ROOT` define

---

## Task 1: vcpkg + `iron::GltfLoader` + tests

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt` (top-level)
- Modify: `engine/CMakeLists.txt`
- Create: `engine/asset/GltfLoader.h`
- Create: `engine/asset/GltfLoader.cpp`
- Create: `tests/assets/gltf/Box.gltf`, `Box0.bin`
- Create: `tests/assets/gltf/BoxTextured.gltf` + textures
- Create: `tests/assets/gltf/Triangle.gltf`
- Create: `tests/test_gltf_loader.cpp`
- Modify: `tests/CMakeLists.txt`

Standalone task — after this, the engine has a working glTF loader + tests. No game uses it yet.

- [ ] **Step 1: Add `tinygltf` to vcpkg manifest**

Open `vcpkg.json`. Add `"tinygltf"` alongside the existing dependencies:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "joltphysics",
    "tinygltf",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

- [ ] **Step 2: `find_package(tinygltf)` in top-level CMakeLists.txt**

Open `CMakeLists.txt`. Find the existing `find_package(Jolt CONFIG REQUIRED)` line (added in M18). Add immediately after:

```cmake
find_package(tinygltf CONFIG REQUIRED)
```

- [ ] **Step 3: Register `GltfLoader.cpp` + link tinygltf PRIVATE in engine CMake**

Open `engine/CMakeLists.txt`. In the `add_library(ironcore STATIC ...)` block, find the line `physics/CharacterController.cpp` (added in M19) and add immediately after:

```cmake
  asset/GltfLoader.cpp
```

Find the existing `target_link_libraries(ironcore PRIVATE unofficial::joltphysics::Jolt)` or `target_link_libraries(ironcore PRIVATE Jolt::Jolt)` block (from M18). Add another line in the same block (or a sibling line if the block is a single call):

```cmake
target_link_libraries(ironcore PRIVATE
  Jolt::Jolt
  tinygltf::tinygltf)
```

(Adjust based on the existing structure — keep the existing Jolt link, add `tinygltf::tinygltf` in the same PRIVATE block.)

- [ ] **Step 4: Write `engine/asset/GltfLoader.h`**

```cpp
#pragma once

#include "scene/Mesh.h"

#include <optional>
#include <string>

namespace iron {

// Loads the first primitive of the first mesh of the first node of the
// default scene from a glTF or GLB file. Returns std::nullopt on parse
// failure, missing required attributes (POSITION, NORMAL, or indices),
// or unsupported accessor types.
//
// `path` can be .gltf (with adjacent .bin) or .glb (single binary file).
// tinygltf auto-detects from the extension.
//
// Attribute mapping (glTF → engine Vertex):
//   POSITION   (vec3 float)  → Vertex::position    [required]
//   NORMAL     (vec3 float)  → Vertex::normal      [required]
//   TEXCOORD_0 (vec2 float)  → Vertex::uv          [optional; defaults to (0,0)]
//   TANGENT    (vec4 float)  → Vertex::tangent     [optional; xyz only, w sign dropped;
//                                                   defaults to (1,0,0)]
//   indices    (u16 or u32)  → u32 indices         [required; u16 promoted to u32]
//
// No texture / material / skin / animation data is loaded — M22 scope
// is geometry only. Game code passes engine default textures
// (whiteTexture / flatNormalTexture / noSpecularTexture).
std::optional<MeshData> loadGltfMesh(const std::string& path);

}  // namespace iron
```

- [ ] **Step 5: Write `engine/asset/GltfLoader.cpp`**

```cpp
// GltfLoader.cpp — tinygltf-backed loader for a single static primitive.
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
// We don't load images in M22 — disable image loading entirely.
#include <tiny_gltf.h>

#include <cstdint>
#include <cstring>
#include <filesystem>

namespace iron {

namespace {

// Read a vec3 accessor from the model. Returns empty on error.
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

// Read a vec2 accessor.
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

// Read a vec4 accessor (used for TANGENT; we drop the w sign).
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

// Read an index accessor, promoting u16 → u32.
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
                return {};  // unsupported component type
        }
        out.push_back(idx);
    }
    return out;
}

}  // namespace

std::optional<MeshData> loadGltfMesh(const std::string& path) {
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

    // Walk to the first primitive of the first mesh of the first node of
    // the default scene.
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

    // Find the first node that has a mesh (some glTFs put the mesh
    // under a child node, e.g. the Box sample has scene → node[0] → mesh).
    int meshIdx = -1;
    for (const int nodeIdx : scene.nodes) {
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) continue;
        const auto& node = model.nodes[nodeIdx];
        if (node.mesh >= 0) { meshIdx = node.mesh; break; }
        // Recurse one level: glTF often has scene.nodes[i].children[j].mesh
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

    // Required: POSITION + NORMAL + indices.
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

    // Optional: TEXCOORD_0, TANGENT.
    std::vector<Vec2> uvs;
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
        uvs = readVec2Accessor(model, uvIt->second);
        if (uvs.size() != n) uvs.clear();  // malformed; fall back to defaults
    }

    std::vector<Vec3> tangents;
    auto tanIt = prim.attributes.find("TANGENT");
    if (tanIt != prim.attributes.end()) {
        tangents = readVec4AsVec3Accessor(model, tanIt->second);
        if (tangents.size() != n) tangents.clear();
    }

    // Indices.
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

    Log::info("GltfLoader: loaded %s — %zu verts, %zu indices",
              path.c_str(), out.vertices.size(), out.indices.size());
    return out;
}

}  // namespace iron
```

- [ ] **Step 6: Vendor Khronos sample glTFs into `tests/assets/gltf/`**

Create the directory and copy in three Khronos CC0 samples. The minimal set:

```
tests/assets/gltf/
├── Box.gltf              # JSON only; references Box0.bin
├── Box0.bin              # 840 bytes of vertex/index data
├── BoxTextured.gltf      # adds UVs + a texture reference (texture ignored by loader)
├── CesiumLogoFlat.png    # the texture (unused but referenced by BoxTextured)
└── Triangle.gltf         # single-file with embedded buffer (data: URI)
```

Source: <https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Box>, `BoxTextured`, `Triangle`.

> **For the implementer:** download these three samples from the upstream repo. Each is < 5 KB except the texture (~7 KB). Total < 20 KB committed. Use `Box/glTF/Box.gltf`, `Box/glTF/Box0.bin`, `BoxTextured/glTF/BoxTextured.gltf`, `BoxTextured/glTF/Box0.bin` (same data as Box's), `BoxTextured/glTF/CesiumLogoFlat.png`, `Triangle/glTF/Triangle.gltf`. The .gltf file contents are JSON; safe to commit.
>
> If you can't download external files, fall back to **writing minimal hand-crafted glTFs from scratch** — Triangle.gltf can be done in ~20 lines of JSON with an embedded base64 buffer. Box is similar. Email me back if blocked.

- [ ] **Step 7: Write `tests/test_gltf_loader.cpp`**

```cpp
#include "asset/GltfLoader.h"
#include "test_framework.h"

#include <cmath>
#include <string>

int main() {
    using namespace iron;

    const std::string base = std::string(IRON_REPO_ROOT) + "/tests/assets/gltf";

    // --- Box.gltf: 24 vertices, 36 indices (one cube primitive) ---
    {
        auto data = loadGltfMesh(base + "/Box.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 24);
        CHECK(data->indices.size()  == 36);
        // Sanity-check: all positions inside the unit-cube AABB.
        for (const auto& v : data->vertices) {
            CHECK(std::fabs(v.position.x) <= 0.6f);
            CHECK(std::fabs(v.position.y) <= 0.6f);
            CHECK(std::fabs(v.position.z) <= 0.6f);
        }
    }

    // --- BoxTextured.gltf: same geometry, UVs populated ---
    {
        auto data = loadGltfMesh(base + "/BoxTextured.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 24);
        bool sawNonZeroUv = false;
        for (const auto& v : data->vertices) {
            if (v.uv.x != 0.0f || v.uv.y != 0.0f) { sawNonZeroUv = true; break; }
        }
        CHECK(sawNonZeroUv);
    }

    // --- Triangle.gltf: 3 verts, 3 indices ---
    {
        auto data = loadGltfMesh(base + "/Triangle.gltf");
        CHECK(data.has_value());
        CHECK(data->vertices.size() == 3);
        CHECK(data->indices.size()  == 3);
    }

    // --- Invalid path returns nullopt ---
    {
        auto data = loadGltfMesh("/this/path/does/not/exist.gltf");
        CHECK(!data.has_value());
    }

    return iron_test_result();
}
```

- [ ] **Step 8: Register `test_gltf_loader` in `tests/CMakeLists.txt`**

After the `iron_add_test(test_character_controller test_character_controller.cpp)` line (added in M19), add:

```cmake
iron_add_test(test_gltf_loader test_gltf_loader.cpp)
target_compile_definitions(test_gltf_loader PRIVATE
  IRON_REPO_ROOT="${CMAKE_SOURCE_DIR}")
```

(Same pattern as `test_texture_loader`.)

- [ ] **Step 9: Configure + build**

```
cmake -S . -B build-vk
cmake --build build-vk --config Debug --target ironcore test_gltf_loader
```

Expected: vcpkg installs `tinygltf` (cold ~30s, warm cache instant). Clean compile.

Common issues:
- `TINYGLTF_IMPLEMENTATION` defined more than once — should only be in `GltfLoader.cpp`. Don't include `<tiny_gltf.h>` anywhere else.
- `Triangle.gltf` parse error — the embedded data URI must be valid base64; copy the file exactly from Khronos's source.
- `stb_image.h` redefinition — the `TINYGLTF_NO_STB_IMAGE` defines should prevent it. If errors persist, also add `#define STBI_NO_THREAD_LOCALS` before the include.

- [ ] **Step 10: Run the test**

```
ctest --test-dir build-vk -C Debug -R test_gltf_loader --output-on-failure
```

Expected: PASS. All 4 sub-tests.

If "Box.gltf vertex count != 24" fails: the Khronos Box.gltf uses 24 vertices (4 per face × 6 faces) to support per-face normals. If a different version is vendored (some have 8 shared-vertex variants), the expected count differs.

- [ ] **Step 11: Commit**

```
git add vcpkg.json CMakeLists.txt engine/CMakeLists.txt \
        engine/asset/GltfLoader.h engine/asset/GltfLoader.cpp \
        tests/assets/gltf/Box.gltf tests/assets/gltf/Box0.bin \
        tests/assets/gltf/BoxTextured.gltf tests/assets/gltf/CesiumLogoFlat.png \
        tests/assets/gltf/Triangle.gltf \
        tests/test_gltf_loader.cpp tests/CMakeLists.txt
git commit -m "M22 Task 1: glTF loader (tinygltf via vcpkg) + Khronos CC0 test assets"
```

---

## Task 2: `games/10-gltf-viewer` demo

**Files:**
- Modify: `CMakeLists.txt` (top-level) — add `add_subdirectory(games/10-gltf-viewer)`
- Create: `games/10-gltf-viewer/CMakeLists.txt`
- Create: `games/10-gltf-viewer/main.cpp`
- Create: `games/10-gltf-viewer/assets/damaged-helmet/DamagedHelmet.gltf` (+ `.bin` + textures, all CC0 from Khronos)

Visual validator. Vulkan-only.

- [ ] **Step 1: Add the subdirectory in top-level CMake**

Open `CMakeLists.txt` (top-level). Find `add_subdirectory(games/09-physics-playground)`. Add immediately after:

```cmake
add_subdirectory(games/10-gltf-viewer)
```

- [ ] **Step 2: Write `games/10-gltf-viewer/CMakeLists.txt`**

```cmake
if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(gltf-viewer main.cpp)
    target_link_libraries(gltf-viewer PRIVATE ironcore)

    # Copy the vendored Damaged Helmet next to the built exe.
    add_custom_command(TARGET gltf-viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:gltf-viewer>/assets
      COMMENT "Copying glTF assets next to gltf-viewer")
endif()
```

(Same pattern as `games/07-net-shooter/CMakeLists.txt`.)

- [ ] **Step 3: Vendor the Damaged Helmet assets**

Download from <https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet/glTF>. Required files:

```
games/10-gltf-viewer/assets/damaged-helmet/
├── DamagedHelmet.gltf
├── DamagedHelmet.bin
├── Default_albedo.jpg
├── Default_AO.jpg
├── Default_emissive.jpg
├── Default_metalRoughness.jpg
└── Default_normal.jpg
```

Total ~250 KB. Commit all of them — M22 doesn't load textures but they're referenced by the .gltf file (tinygltf logs a warning if missing, but loading still succeeds because we disabled image loading).

> If downloading isn't available, fall back to writing a tiny test glTF inline. The Damaged Helmet is preferred for visual impact but Box.glb works as a substitute.

- [ ] **Step 4: Write `games/10-gltf-viewer/main.cpp`**

```cpp
// games/10-gltf-viewer/main.cpp — Vulkan-only static-mesh viewer for
// glTF files. Loads the Khronos Damaged Helmet CC0 sample by default.
// Free-fly camera; WASD + mouse + Space/Ctrl. ESC to quit.

#include "asset/GltfLoader.h"
#include "core/Application.h"
#include "core/Input.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "scene/FreeFlyCamera.h"
#include "scene/Mesh.h"
#include "ui/Hud.h"
#include "ui/BuiltinFont.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <numbers>
#include <span>
#include <string>

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

#ifdef IRON_RENDER_BACKEND_VULKAN

// Lit shader copied from net-shooter (M19/M20/M21 layout). Reused
// here for the static mesh.
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;
layout(set = 0, binding = 5) uniform samplerCube uSkyCubemap;
layout(set = 0, binding = 6) uniform sampler2D uReflection;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = -normalize(u.sunDir.xyz);
    float diffuse = max(dot(N, L), 0.0);
    vec3 diff = texture(uDiffuse, vUV).rgb;
    vec3 lit = diff * (u.sunColor.xyz * diffuse + u.ambient.xyz) + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
)";

#endif  // IRON_RENDER_BACKEND_VULKAN

}  // namespace

int main() {
    iron::Application::Config cfg;
    cfg.title  = "Iron Core - glTF Viewer";
    cfg.width  = kScreenW;
    cfg.height = kScreenH;
    iron::Application app(cfg);
    if (!app.valid()) {
        iron::Log::error("gltf-viewer: Application init failed");
        return 1;
    }

    auto renderer_ptr = iron::createRenderer(app.window());
    if (!renderer_ptr) {
        iron::Log::error("gltf-viewer: renderer init failed");
        return 1;
    }
    iron::Renderer& renderer = *renderer_ptr;
    renderer.setViewport(kScreenW, kScreenH);

    // Skybox: 1×1 black cubemap so binding 5 has a valid sampler
    // (same trick the physics-playground uses).
    {
        const unsigned char black[4] = {0, 0, 0, 255};
        std::array<const unsigned char*, 6> faces = {black, black, black, black, black, black};
        iron::CubemapHandle sky = renderer.createCubemap(1, 1, faces);
        renderer.setSkybox(sky);
    }

    // Load the model.
    const std::string modelPath = iron::executableDir()
        + "/assets/damaged-helmet/DamagedHelmet.gltf";
    auto data = iron::loadGltfMesh(modelPath);
    if (!data) {
        iron::Log::error("gltf-viewer: failed to load %s", modelPath.c_str());
        return 1;
    }
    iron::Log::info("gltf-viewer: loaded %zu verts, %zu indices",
                    data->vertices.size(), data->indices.size());

    const iron::MeshHandle mesh = renderer.createMesh(*data);
    const iron::ShaderHandle shader = renderer.createShader(kVertexShader, kFragmentShader);
    if (mesh == iron::kInvalidHandle || shader == iron::kInvalidHandle) {
        iron::Log::error("gltf-viewer: mesh/shader create failed");
        return 1;
    }

    iron::FreeFlyCamera cam;
    cam.position = {0.0f, 0.0f, 3.0f};

    const float aspect = static_cast<float>(kScreenW) / static_cast<float>(kScreenH);
    const iron::Mat4 proj = iron::perspective(
        cam.fovDeg * (std::numbers::pi_v<float> / 180.0f),
        aspect, 0.1f, 100.0f);

    app.window().setCursorCaptured(true);

    iron::BitmapFont font = iron::builtinFont(
        renderer.createTexture(iron::builtinFontAtlas().width,
                                iron::builtinFontAtlas().height,
                                iron::builtinFontAtlas().pixels.data()));
    iron::Hud hud;
    char statsBuf[128];
    std::snprintf(statsBuf, sizeof(statsBuf),
                  "Verts: %zu  Tris: %zu",
                  data->vertices.size(), data->indices.size() / 3);
    hud.addText(statsBuf, iron::Vec2{10, 10}, 1.0f,
                iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    hud.addText("WASD: move  mouse: look  Space/Ctrl: up/down  ESC: quit",
                iron::Vec2{10, static_cast<float>(kScreenH - 24)}, 1.0f,
                iron::Vec4{1.0f, 1.0f, 0.0f, 1.0f});

    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);
        }
        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());
        cam.update(t.deltaSeconds, mdx, mdy,
                   input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                   input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                   input.keyDown(GLFW_KEY_LEFT_CONTROL),
                   input.keyDown(GLFW_KEY_SPACE),
                   3.0f);
    });

    app.setRender([&]() {
        const iron::Mat4 view = cam.viewMatrix();
        iron::DirectionalLight sun;
        sun.direction = iron::normalize(iron::Vec3{-0.4f, -1.0f, -0.3f});
        sun.color     = {1.0f, 0.92f, 0.80f};
        sun.ambient   = 0.25f;

        renderer.beginFrame({0.10f, 0.10f, 0.12f}, sun,
                            std::span<const iron::PointLight>{},
                            iron::Fog{}, view, proj);

        iron::DrawCall call;
        call.mesh   = mesh;
        call.shader = shader;
        call.model  = iron::Mat4::identity();
        call.material.texture     = renderer.whiteTexture();
        call.material.normalMap   = renderer.flatNormalTexture();
        call.material.specularMap = renderer.noSpecularTexture();
        call.material.emissive    = iron::Vec3{0.05f, 0.05f, 0.05f};
        renderer.submit(call);

        renderer.drawHud(hud.build(font, renderer.whiteTexture()),
                         kScreenW, kScreenH);
        renderer.endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
}
```

> **Notes:**
> - The shader strings are pasted from net-shooter to match the post-M17 LitUbo layout. If they differ slightly (newer field additions), copy from `games/07-net-shooter/main.cpp` to keep them in sync.
> - `iron::builtinFontAtlas()` and `iron::builtinFont(textureHandle)` are free functions from `engine/ui/BuiltinFont.h` (per M18 implementer's discovery). If the names differ, look at how `games/09-physics-playground/main.cpp` loads its font.
> - Damaged Helmet is centered at origin with ~2m extent. Camera at (0, 0, 3) sees it well.

- [ ] **Step 5: Build**

```
cmake --build build-vk --config Debug --target gltf-viewer
```

Expected: clean compile.

- [ ] **Step 6: Smoke test**

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe
```

Expected:
- Window opens, dim background.
- Damaged Helmet visible as a white-shaded mesh (no textures applied — flat-looking but recognizable as a helmet shape).
- WASD + mouse to fly around it.
- HUD shows vert/tri counts (~14000 verts, ~15000 tris).
- ESC quits.

If the model is invisible: check the load log for errors. If the model is at a different position or scale, the camera may not see it; tweak `cam.position`.

If the load logs "no mesh found in scene": the loader's scene-walk might not handle Damaged Helmet's specific node structure. Add a deeper recursion or pick the first non-empty mesh in `model.meshes`. Adjust the loader if needed (this is a real risk — Damaged Helmet's structure is `scene → node → child node → mesh`, which the loader handles, but if it doesn't work, escalate).

- [ ] **Step 7: Commit**

```
git add CMakeLists.txt games/10-gltf-viewer/
git commit -m "M22 Task 2: 10-gltf-viewer demo loading Damaged Helmet (CC0)"
```

---

## Task 3: Docs + PR

**Files:**
- Create: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Write `docs/engine/asset-pipeline.md`**

```markdown
# Asset pipeline

This is the home for engine-side asset import. Currently it covers
glTF mesh loading (M22+); future tracks will add texture import polish,
audio loading, level format, etc.

## glTF loader (`iron::loadGltfMesh`)

`engine/asset/GltfLoader.{h,cpp}` ships a single-function API:

```cpp
std::optional<MeshData> loadGltfMesh(const std::string& path);
```

Backed by [tinygltf](https://github.com/syoyo/tinygltf) via vcpkg.
Loads the first primitive of the first mesh of the first node of the
default scene from a `.gltf` (JSON + adjacent `.bin`) or `.glb` (single
binary) file. `tinygltf` headers are confined to the `.cpp`; the
library is linked PRIVATE to `ironcore` so game code never includes it.

### Vertex attribute mapping

| glTF attribute  | Engine `Vertex`     | Required | Fallback         |
| --------------- | ------------------- | -------- | ---------------- |
| `POSITION`      | `position` (Vec3)   | yes      | hard-fail        |
| `NORMAL`        | `normal` (Vec3)     | yes      | hard-fail        |
| `TEXCOORD_0`    | `uv` (Vec2)         | no       | `(0, 0)`         |
| `TANGENT`       | `tangent` (Vec3)    | no       | `(1, 0, 0)`      |
| `indices`       | `indices` (u32)     | yes      | hard-fail        |

`TANGENT` is `vec4` in glTF (the w stores the handedness sign); we drop
w and store the xyz. Index buffers come as `u16` or `u32` in glTF; the
loader promotes `u16` to `u32` at load time so the engine's mesh
interface is always `std::uint32_t`.

### What's NOT loaded in M22

- **Textures / materials.** The loader skips images entirely (tinygltf
  image loading is disabled). Game code passes engine defaults:
  `renderer.whiteTexture()` for diffuse, `flatNormalTexture()` for
  normal map, `noSpecularTexture()` for specular. Textures land in a
  small follow-up or as part of M23.
- **Skeletons, joints, skinning data** — M23.
- **Animations** — M24.
- **Multi-primitive meshes, multi-mesh scenes, scene-graph traversal
  beyond `scene[default].nodes[0].mesh` (with one level of recursion
  into children)** — extended in later milestones if needed.
- **MikkTSpace tangent synthesis** for models that don't ship tangents
  — fallback `(1, 0, 0)` is used; visual artifact only with normal
  maps.
- **glTF 2.0 extensions** (`KHR_draco_compression`, `KHR_texture_basisu`,
  `KHR_lights_punctual`, etc.) — not supported.

### Visual validator: `games/10-gltf-viewer`

Loads the Khronos "Damaged Helmet" CC0 sample. Vulkan-only; free-fly
camera; HUD shows vert/tri counts. Run with:

```
.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe
```

Helmet renders flat-shaded white (no textures) but with correct
geometry — proves the loader works end-to-end.

## What's next

- **M23** — Skeleton + GPU skinning shader. Extend the loader to read
  `skin` data; new `iron::Skeleton` + `iron::SkinnedMesh` types; new
  Vulkan skinning vertex shader. Render the same model with its
  skeleton in bind pose.
- **M24** — Animation curves + playback. Sample animation curves at
  runtime to compute bone transforms.
- **M25** — Wire skinned characters into net-shooter (idle/walk
  anims, ragdoll bones drive the skinned mesh on death).
- Follow-up between M22 and M23: load base-color (+ normal + spec)
  textures from glTF and feed them into the existing `Material`
  fields. ~100 LOC; small fixup-style task.
```

- [ ] **Step 2: Commit**

```
git add docs/engine/asset-pipeline.md
git commit -m "M22 Task 3: docs/engine/asset-pipeline.md (glTF loader + track plan)"
```

- [ ] **Step 3: Push + open PR**

```
git push -u origin feat/m22-static-gltf-mesh-import
gh pr create --title "M22: Static glTF mesh import + 10-gltf-viewer demo" --body "$(cat <<'EOF'
## Summary
- New `iron::loadGltfMesh(path)` engine API (`engine/asset/GltfLoader.{h,cpp}`) backed by tinygltf via vcpkg
- Loads geometry only — positions/normals/UVs/tangents/indices. Maps to existing `iron::MeshData` directly.
- Textures, skeletons, animations explicitly deferred to M23/M24/M25
- New `games/10-gltf-viewer` Vulkan demo loading Khronos "Damaged Helmet" CC0 sample
- 4 new unit tests (Box.gltf, BoxTextured.gltf, Triangle.gltf, invalid path)
- Vendored Khronos CC0 test assets (Box, BoxTextured, Triangle) and Damaged Helmet
- New `docs/engine/asset-pipeline.md`

First milestone of the M22-M25 skeletal-animation + glTF track. After this lands the engine has a working foundation for loading 3D models.

## Test plan
- [ ] CI green (Windows MSVC) — including the 4 new sub-tests
- [ ] `.\build-vk\games\10-gltf-viewer\Debug\gltf-viewer.exe` shows the Damaged Helmet (flat-shaded white, no textures)
- [ ] Free-fly camera works
- [ ] No regression in existing engine tests

## Known v1 limitations
- No textures / materials loaded (game uses engine defaults)
- No skeleton / animations
- Single primitive only (first of first mesh of first node)
- TANGENT fallback is `(1, 0, 0)` for models without baked tangents

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- vcpkg + find_package + PRIVATE link → Task 1 Steps 1-3
- `iron::loadGltfMesh` API → Task 1 Steps 4-5
- glTF attribute mapping (required + optional + defaults) → Task 1 Step 5 (loader impl)
- u16 → u32 index promotion → Task 1 Step 5 (`readIndicesAccessor`)
- Vendored test glTFs → Task 1 Step 6
- 4 unit tests → Task 1 Step 7
- 10-gltf-viewer demo → Task 2
- Damaged Helmet vendored → Task 2 Step 3
- Docs → Task 3

**Placeholder scan:** clean — every code step has full content. The Khronos asset downloads are flagged with a fallback ("hand-craft a minimal Triangle.gltf").

**Type consistency:**
- `iron::MeshData`, `iron::Vertex`, `iron::Vec3`, `iron::Vec2` consistent across loader + tests + demo
- `loadGltfMesh` signature stays the same across header, impl, tests, demo
- `tinygltf::tinygltf` CMake target name used consistently

**Known v1 limitations called out:** texture loading deferred, scene-graph traversal limited to one level of child recursion, no MikkTSpace synthesis.
