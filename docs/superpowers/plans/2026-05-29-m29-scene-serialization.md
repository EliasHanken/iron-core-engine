# M29: Scene Serialization + Sandbox Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Define a JSON scene file format (renderable entities + lighting), a headlessly-testable load/save library, and a `games/11-sandbox` runtime that loads a scene file and renders it with a free-fly camera.

**Architecture:** A new `iron::SceneFile` POD model (entities reference meshes/textures by path or primitive name, never by GPU handle) lives in `engine/scene/SceneFormat.h`. `engine/scene/SceneIO.{h,cpp}` serializes it to/from JSON via a vendored nlohmann/json single header — pure data, no GPU calls, so the round-trip is unit-tested headlessly. `games/11-sandbox` resolves the path references to runtime handles (primitives via `makeCube`/`appendQuad`, glTF via `loadGltfModel`, textures via `loadTexture`) and renders with the existing lit pipeline + free-fly camera.

**Tech Stack:** C++23 (`/std:c++latest`), Vulkan 1.3, nlohmann/json (vendored single header), CMake, CTest. Builds on M22 (`loadGltfModel`), the lit shader pipeline, and `FreeFlyCamera`.

**Spec:** `docs/superpowers/specs/2026-05-29-m29-scene-serialization-design.md`

**Correction vs spec:** `iron::DirectionalLight` already has a `float ambient` member (engine/render/Light.h), and `Renderer::beginFrame` takes the `DirectionalLight` directly. So `SceneFile` does NOT carry a separate `Vec3 ambient` — ambient lives on `sun.ambient`. The plan below reflects this.

**Prerequisite:** M28 merged on `main` (commit `97540b7`). Branch `feat/m29-scene-serialization` is cut off main with the spec committed.

---

## Real struct members (ground truth — match these exactly)

```cpp
// engine/render/Light.h
struct DirectionalLight { Vec3 direction{0,-1,0}; Vec3 color{1,1,1}; float ambient = 0.1f; };
struct PointLight { Vec3 position{0,0,0}; Vec3 color{1,1,1}; float intensity = 1.0f; float range = 5.0f; };
// engine/render/Fog.h
struct Fog { Vec3 color{0.7f,0.6f,0.5f}; float density = 0.0f; };
// engine/math/Vec.h — Vec3{x,y,z}, Vec2{x,y}
// engine/math/Quaternion.h — Quat{x,y,z,w}, Quat::identity()
// engine/scene/Mesh.h — MeshData makeCube();  void appendQuad(MeshData&, Vec3 center, Vec2 size, Vec3 normal);
// Renderer::beginFrame(Vec3 clearColor, const DirectionalLight&, std::span<const PointLight>, const Fog&, const Mat4& view, const Mat4& proj)
```

---

## File Structure

**Create:**
- `third_party/json/json.hpp` — vendored nlohmann/json single header.
- `third_party/json/CMakeLists.txt` — INTERFACE target `nlohmann_json`.
- `engine/scene/SceneFormat.h` — `SceneFile` / `SceneEntity` / `MeshRef` / `MaterialDef` / `PrimitiveKind` POD types.
- `engine/scene/SceneIO.h` — `loadSceneFile` / `saveSceneFile` declarations.
- `engine/scene/SceneIO.cpp` — JSON serialize/deserialize.
- `tests/test_scene_io.cpp` — round-trip + malformed + defaults tests.
- `games/11-sandbox/main.cpp` — the runtime.
- `games/11-sandbox/CMakeLists.txt` — target + POST_BUILD asset copy.
- `games/11-sandbox/assets/scenes/demo.json` — hand-authored demo scene.
- `games/11-sandbox/assets/damaged-helmet/*` — vendored CC0 glTF (copy of 10-gltf-viewer's).
- `docs/engine/scene-format.md` — format reference doc.

**Modify:**
- `third_party/CMakeLists.txt` — `add_subdirectory(json)`.
- `engine/CMakeLists.txt` — add `scene/SceneIO.cpp`; link `nlohmann_json` PRIVATE.
- `tests/CMakeLists.txt` — register `test_scene_io`.
- `CMakeLists.txt` (root) — `add_subdirectory(games/11-sandbox)`.

**Unchanged (deliberately):** `engine/scene/Scene.h` (legacy; strandbound keeps using it).

---

## Task 1: Vendor nlohmann/json

**Files:**
- Create: `third_party/json/json.hpp`
- Create: `third_party/json/CMakeLists.txt`
- Modify: `third_party/CMakeLists.txt`

- [ ] **Step 1: Download the single-header release**

```powershell
$dest = 'C:\Users\elias\Documents\_dev\iron-core-engine\third_party\json'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$src = 'https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp'
Invoke-WebRequest -Uri $src -OutFile (Join-Path $dest 'json.hpp')
```

Verify (~900 KB, starts with the library banner):

```powershell
Get-Item (Join-Path $dest 'json.hpp') | Select-Object Length
Get-Content (Join-Path $dest 'json.hpp') -TotalCount 5
```

Expected: ~900000-950000 bytes; header comment mentioning "JSON for Modern C++".

If the tag URL 404s, use `develop` branch: `https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp`.

- [ ] **Step 2: Create `third_party/json/CMakeLists.txt`**

```cmake
# nlohmann/json — single-header JSON library (MIT). Used by engine/scene/SceneIO
# for scene file serialization. Header-only; no link step.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Register the subdir in `third_party/CMakeLists.txt`**

Read the current file (it has `stb_image` + the M28 `add_subdirectory(dr_libs)`). Append:

```cmake

add_subdirectory(json)
```

- [ ] **Step 4: Reconfigure to confirm the target resolves**

```powershell
cmake -S . -B build-vk
```

Expected: configure succeeds, no error mentioning `json` (INTERFACE target won't build until something links it).

- [ ] **Step 5: Commit**

```bash
git add third_party/json/json.hpp third_party/json/CMakeLists.txt third_party/CMakeLists.txt
git commit -m "M29 Task 1: vendor nlohmann/json single header"
```

---

## Task 2: SceneFormat types + SceneIO (TDD)

**Files:**
- Create: `engine/scene/SceneFormat.h`
- Create: `engine/scene/SceneIO.h`
- Create: `engine/scene/SceneIO.cpp`
- Create: `tests/test_scene_io.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `engine/scene/SceneFormat.h`**

```cpp
#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"

#include <optional>
#include <string>
#include <vector>

namespace iron {

// A builtin procedural mesh. v1 supports the two the engine already has
// builders for (makeCube / appendQuad). Sphere etc. are future additions.
enum class PrimitiveKind { Cube, Plane };

// How an entity gets its geometry: a builtin primitive OR a static
// (non-skinned) glTF path. If `primitive` is set it wins; otherwise
// `gltfPath` is loaded. If neither resolves, the loader logs and skips
// the entity (the rest of the scene still renders).
struct MeshRef {
    std::optional<PrimitiveKind> primitive;
    std::string                  gltfPath;
};

// Surface appearance. Texture paths resolve to engine textures at load;
// "" means "use the engine's builtin default" (white / flat-normal / no-spec).
struct MaterialDef {
    std::string albedoPath;
    std::string normalPath;
    std::string specularPath;
    Vec3        emissive     = {0.0f, 0.0f, 0.0f};
    float       uvScale      = 1.0f;
    float       reflectivity = 0.0f;
};

// One placed object: a transform + what to draw.
struct SceneEntity {
    std::string name;
    Vec3        position = {0.0f, 0.0f, 0.0f};
    Quat        rotation = Quat::identity();   // serialized as [x, y, z, w]
    Vec3        scale    = {1.0f, 1.0f, 1.0f};
    MeshRef     mesh;
    MaterialDef material;
};

// A complete authored scene: placed entities + global lighting/environment.
// Ambient lives on `sun.ambient` (DirectionalLight carries it) — there is
// no separate scene-level ambient.
struct SceneFile {
    std::vector<SceneEntity> entities;
    DirectionalLight         sun;
    std::vector<PointLight>  pointLights;
    Fog                      fog;
    Vec3                     clearColor = {0.5f, 0.6f, 0.7f};
};

}  // namespace iron
```

- [ ] **Step 2: Create `engine/scene/SceneIO.h`**

```cpp
#pragma once

#include "scene/SceneFormat.h"

#include <optional>
#include <string>

namespace iron {

// Load a scene from a JSON file. Returns nullopt on a missing file or
// malformed JSON (logs the error via Log::error). Missing optional fields
// fall back to the SceneFile / SceneEntity struct defaults.
std::optional<SceneFile> loadSceneFile(const std::string& path);

// Write a scene to a JSON file (pretty-printed, 2-space indent, for
// human-diffable output). Returns false if the file can't be opened.
bool saveSceneFile(const SceneFile& scene, const std::string& path);

}  // namespace iron
```

- [ ] **Step 3: Write failing tests in `tests/test_scene_io.cpp`**

Use the project's single-`main` harness (`CHECK` / `CHECK_NEAR`, ends with `return iron_test_result();`). Match `tests/test_animation_player.cpp` / `tests/test_audio_engine.cpp` style exactly.

```cpp
#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace iron;
namespace fs = std::filesystem;

namespace {

std::string tempScenePath(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

SceneFile makeSampleScene() {
    SceneFile s;
    s.clearColor = {0.2f, 0.3f, 0.4f};
    s.sun.direction = {-0.4f, -1.0f, -0.3f};
    s.sun.color     = {1.0f, 0.95f, 0.9f};
    s.sun.ambient   = 0.15f;
    s.fog.color   = {0.5f, 0.5f, 0.6f};
    s.fog.density = 0.02f;

    PointLight pl;
    pl.position  = {0.0f, 3.0f, 0.0f};
    pl.color     = {1.0f, 0.5f, 0.2f};
    pl.intensity = 2.0f;
    pl.range     = 12.0f;
    s.pointLights.push_back(pl);

    SceneEntity floor;
    floor.name     = "floor";
    floor.position = {0.0f, 0.0f, 0.0f};
    floor.scale    = {20.0f, 1.0f, 20.0f};
    floor.mesh.primitive = PrimitiveKind::Plane;
    floor.material.uvScale = 8.0f;
    s.entities.push_back(floor);

    SceneEntity helmet;
    helmet.name     = "helmet";
    helmet.position = {2.0f, 1.5f, 0.0f};
    helmet.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
    helmet.scale    = {1.5f, 1.5f, 1.5f};
    helmet.mesh.gltfPath        = "assets/damaged-helmet/DamagedHelmet.gltf";
    helmet.material.emissive    = {0.1f, 0.0f, 0.0f};
    helmet.material.reflectivity = 0.3f;
    s.entities.push_back(helmet);
    return s;
}

}  // namespace

// --- Test 1: round-trip preserves every field ---
{
    const SceneFile original = makeSampleScene();
    const std::string path = tempScenePath("iron_scene_roundtrip.json");
    CHECK(saveSceneFile(original, path));

    const auto loadedOpt = loadSceneFile(path);
    CHECK(loadedOpt.has_value());
    if (loadedOpt.has_value()) {
        const SceneFile& l = *loadedOpt;
        CHECK_NEAR(l.clearColor.x, 0.2f, 1e-5f);
        CHECK_NEAR(l.clearColor.z, 0.4f, 1e-5f);
        CHECK_NEAR(l.sun.direction.y, -1.0f, 1e-5f);
        CHECK_NEAR(l.sun.ambient, 0.15f, 1e-5f);
        CHECK_NEAR(l.fog.density, 0.02f, 1e-5f);
        CHECK(l.pointLights.size() == 1u);
        if (!l.pointLights.empty()) {
            CHECK_NEAR(l.pointLights[0].range, 12.0f, 1e-5f);
            CHECK_NEAR(l.pointLights[0].intensity, 2.0f, 1e-5f);
        }
        CHECK(l.entities.size() == 2u);
        if (l.entities.size() == 2u) {
            // Entity 0: primitive plane.
            CHECK(l.entities[0].name == "floor");
            CHECK(l.entities[0].mesh.primitive.has_value());
            CHECK(l.entities[0].mesh.primitive.value() == PrimitiveKind::Plane);
            CHECK(l.entities[0].mesh.gltfPath.empty());
            CHECK_NEAR(l.entities[0].scale.x, 20.0f, 1e-5f);
            CHECK_NEAR(l.entities[0].material.uvScale, 8.0f, 1e-5f);
            // Entity 1: glTF path, no primitive.
            CHECK(l.entities[1].name == "helmet");
            CHECK(!l.entities[1].mesh.primitive.has_value());
            CHECK(l.entities[1].mesh.gltfPath == "assets/damaged-helmet/DamagedHelmet.gltf");
            CHECK_NEAR(l.entities[1].material.reflectivity, 0.3f, 1e-5f);
            // Rotation quat preserved (xyzw).
            const Quat q = l.entities[1].rotation;
            const Quat e = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
            CHECK_NEAR(q.x, e.x, 1e-4f);
            CHECK_NEAR(q.y, e.y, 1e-4f);
            CHECK_NEAR(q.z, e.z, 1e-4f);
            CHECK_NEAR(q.w, e.w, 1e-4f);
        }
    }
    fs::remove(path);
}

// --- Test 2: malformed JSON returns nullopt ---
{
    const std::string path = tempScenePath("iron_scene_malformed.json");
    { std::ofstream f(path); f << "{ this is not valid json ]"; }
    const auto loaded = loadSceneFile(path);
    CHECK(!loaded.has_value());
    fs::remove(path);
}

// --- Test 3: missing file returns nullopt ---
{
    const auto loaded = loadSceneFile("does/not/exist/scene.json");
    CHECK(!loaded.has_value());
}

// --- Test 4: minimal scene uses defaults for omitted fields ---
{
    const std::string path = tempScenePath("iron_scene_minimal.json");
    {
        std::ofstream f(path);
        f << R"({ "entities": [ { "name": "c", "mesh": { "primitive": "cube" } } ] })";
    }
    const auto loadedOpt = loadSceneFile(path);
    CHECK(loadedOpt.has_value());
    if (loadedOpt.has_value()) {
        const SceneFile& l = *loadedOpt;
        CHECK(l.entities.size() == 1u);
        if (!l.entities.empty()) {
            CHECK(l.entities[0].mesh.primitive.has_value());
            CHECK(l.entities[0].mesh.primitive.value() == PrimitiveKind::Cube);
            // Omitted transform fields default.
            CHECK_NEAR(l.entities[0].position.x, 0.0f, 1e-6f);
            CHECK_NEAR(l.entities[0].scale.x, 1.0f, 1e-6f);
            CHECK_NEAR(l.entities[0].rotation.w, 1.0f, 1e-6f);
        }
        // Omitted scene fields default.
        CHECK_NEAR(l.clearColor.x, 0.5f, 1e-6f);
        CHECK(l.pointLights.empty());
        CHECK_NEAR(l.fog.density, 0.0f, 1e-6f);
    }
    fs::remove(path);
}
```

Wrap each `{ ... }` block in the harness's test convention (a leading `// Test N:` comment + the block, inside `main`). Confirm the exact shape against `tests/test_audio_engine.cpp` before writing — replicate it.

- [ ] **Step 4: Wire CMake (so the test links)**

In `engine/CMakeLists.txt`: add `scene/SceneIO.cpp` to the `add_library(ironcore STATIC ...)` source list (near the other `scene/` entries). After the M28 OpenAL/dr_libs link block, add the json link:

```cmake
# M29 — scene serialization uses nlohmann/json (header-only, vendored).
target_link_libraries(ironcore PRIVATE nlohmann_json)
```

In `tests/CMakeLists.txt`: add `iron_add_test(test_scene_io test_scene_io.cpp)` (match the actual macro arity used by `test_audio_engine`).

- [ ] **Step 5: Run tests to verify they fail**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target test_scene_io --config Debug
```

Expected: link error (loadSceneFile/saveSceneFile unresolved) — `SceneIO.cpp` not yet written.

- [ ] **Step 6: Implement `engine/scene/SceneIO.cpp`**

```cpp
#include "scene/SceneIO.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace iron {

namespace {

using json = nlohmann::json;

// --- small helpers: Vec3 / Quat <-> json arrays ---

json toJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json toJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

// Read a Vec3 from `j[key]` if present + a 3-array, else leave `out` as-is.
void readVec3(const json& j, const char* key, Vec3& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
    }
}

void readQuat(const json& j, const char* key, Quat& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 4) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
        out.w = j[key][3].get<float>();
    }
}

void readFloat(const json& j, const char* key, float& out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
}

void readString(const json& j, const char* key, std::string& out) {
    if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}

const char* primitiveName(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Cube:  return "cube";
        case PrimitiveKind::Plane: return "plane";
    }
    return "cube";
}

// --- serialize ---

json materialToJson(const MaterialDef& m) {
    json j = json::object();
    if (!m.albedoPath.empty())   j["albedoPath"]   = m.albedoPath;
    if (!m.normalPath.empty())   j["normalPath"]   = m.normalPath;
    if (!m.specularPath.empty()) j["specularPath"] = m.specularPath;
    j["emissive"]     = toJson(m.emissive);
    j["uvScale"]      = m.uvScale;
    j["reflectivity"] = m.reflectivity;
    return j;
}

json meshToJson(const MeshRef& m) {
    json j = json::object();
    if (m.primitive.has_value()) {
        j["primitive"] = primitiveName(m.primitive.value());
    } else {
        j["gltfPath"] = m.gltfPath;
    }
    return j;
}

json entityToJson(const SceneEntity& e) {
    json j = json::object();
    j["name"]     = e.name;
    j["position"] = toJson(e.position);
    j["rotation"] = toJson(e.rotation);
    j["scale"]    = toJson(e.scale);
    j["mesh"]     = meshToJson(e.mesh);
    j["material"] = materialToJson(e.material);
    return j;
}

// --- deserialize ---

MaterialDef materialFromJson(const json& j) {
    MaterialDef m;
    readString(j, "albedoPath",   m.albedoPath);
    readString(j, "normalPath",   m.normalPath);
    readString(j, "specularPath", m.specularPath);
    readVec3  (j, "emissive",     m.emissive);
    readFloat (j, "uvScale",      m.uvScale);
    readFloat (j, "reflectivity", m.reflectivity);
    return m;
}

MeshRef meshFromJson(const json& j) {
    MeshRef m;
    if (j.contains("primitive") && j["primitive"].is_string()) {
        const std::string p = j["primitive"].get<std::string>();
        if (p == "cube")       m.primitive = PrimitiveKind::Cube;
        else if (p == "plane") m.primitive = PrimitiveKind::Plane;
        else Log::warn("SceneIO: unknown primitive '%s'; treating as gltf/none", p.c_str());
    }
    readString(j, "gltfPath", m.gltfPath);
    return m;
}

SceneEntity entityFromJson(const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    readVec3  (j, "position", e.position);
    readQuat  (j, "rotation", e.rotation);
    readVec3  (j, "scale",    e.scale);
    if (j.contains("mesh"))     e.mesh     = meshFromJson(j["mesh"]);
    if (j.contains("material")) e.material = materialFromJson(j["material"]);
    return e;
}

}  // namespace

bool saveSceneFile(const SceneFile& scene, const std::string& path) {
    json root = json::object();
    root["clearColor"] = toJson(scene.clearColor);

    json sun = json::object();
    sun["direction"] = toJson(scene.sun.direction);
    sun["color"]     = toJson(scene.sun.color);
    sun["ambient"]   = scene.sun.ambient;
    root["sun"] = sun;

    json fog = json::object();
    fog["color"]   = toJson(scene.fog.color);
    fog["density"] = scene.fog.density;
    root["fog"] = fog;

    json pls = json::array();
    for (const auto& pl : scene.pointLights) {
        json j = json::object();
        j["position"]  = toJson(pl.position);
        j["color"]     = toJson(pl.color);
        j["intensity"] = pl.intensity;
        j["range"]     = pl.range;
        pls.push_back(j);
    }
    root["pointLights"] = pls;

    json ents = json::array();
    for (const auto& e : scene.entities) ents.push_back(entityToJson(e));
    root["entities"] = ents;

    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);   // 2-space pretty print
    return true;
}

std::optional<SceneFile> loadSceneFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        Log::error("SceneIO: parse error in '%s': %s", path.c_str(), e.what());
        return std::nullopt;
    }

    SceneFile scene;
    readVec3(root, "clearColor", scene.clearColor);

    if (root.contains("sun")) {
        const json& sun = root["sun"];
        readVec3 (sun, "direction", scene.sun.direction);
        readVec3 (sun, "color",     scene.sun.color);
        readFloat(sun, "ambient",   scene.sun.ambient);
    }
    if (root.contains("fog")) {
        const json& fog = root["fog"];
        readVec3 (fog, "color",   scene.fog.color);
        readFloat(fog, "density", scene.fog.density);
    }
    if (root.contains("pointLights") && root["pointLights"].is_array()) {
        for (const auto& j : root["pointLights"]) {
            PointLight pl;
            readVec3 (j, "position",  pl.position);
            readVec3 (j, "color",     pl.color);
            readFloat(j, "intensity", pl.intensity);
            readFloat(j, "range",     pl.range);
            scene.pointLights.push_back(pl);
        }
    }
    if (root.contains("entities") && root["entities"].is_array()) {
        for (const auto& j : root["entities"]) {
            scene.entities.push_back(entityFromJson(j));
        }
    }
    return scene;
}

}  // namespace iron
```

Verify `Log::error` / `Log::warn` accept printf-style format (they do — used throughout the engine). Confirm `nlohmann/json.hpp` is found via the `nlohmann_json` include dir (the vendored path is `third_party/json/json.hpp`, so the include `<nlohmann/json.hpp>` requires the header at `third_party/json/nlohmann/json.hpp` OR a plain `<json.hpp>` include if the header sits directly in `third_party/json/`). RESOLVE THIS: the single-header download in Task 1 puts the file at `third_party/json/json.hpp`. So either (a) include `"json.hpp"` (since the INTERFACE target adds `third_party/json/` to the include path), or (b) place the header at `third_party/json/nlohmann/json.hpp` to match the canonical `<nlohmann/json.hpp>` include. Choose (b) for convention: in Task 1, save the header to `third_party/json/nlohmann/json.hpp` instead of `third_party/json/json.hpp`, and keep the INTERFACE include dir at `third_party/json/`. Update Task 1's download destination accordingly if you take (b). Document which you chose.

- [ ] **Step 7: Run tests to verify they pass**

```powershell
cmake --build build-vk --target test_scene_io --config Debug
ctest --test-dir build-vk -C Debug -R scene_io --output-on-failure
```

Expected: all 4 test blocks pass.

- [ ] **Step 8: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 42 + 1 = 43 tests green (M28 baseline was 42).

- [ ] **Step 9: Commit**

```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M29 Task 2: SceneFile format + SceneIO JSON load/save with round-trip tests"
```

---

## Task 3: Sandbox runtime

**Files:**
- Create: `games/11-sandbox/main.cpp`
- Create: `games/11-sandbox/CMakeLists.txt`
- Create: `games/11-sandbox/assets/scenes/demo.json`
- Create: `games/11-sandbox/assets/damaged-helmet/*` (vendored)
- Modify: `CMakeLists.txt` (root)

Use `games/10-gltf-viewer/main.cpp` as the structural template — it already has the Vulkan lit shader strings, `FreeFlyCamera`, the HUD, and the `loadGltfModel` + texture-loading path. The sandbox is "10-gltf-viewer but driven by a scene file instead of one hardcoded model."

- [ ] **Step 1: Vendor the demo glTF**

Copy 10-gltf-viewer's Damaged Helmet into the sandbox's assets:

```powershell
$srcDir = 'C:\Users\elias\Documents\_dev\iron-core-engine\games\10-gltf-viewer\assets\damaged-helmet'
$dstDir = 'C:\Users\elias\Documents\_dev\iron-core-engine\games\11-sandbox\assets\damaged-helmet'
New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -Recurse -Force "$srcDir\*" $dstDir
Get-ChildItem $dstDir
```

(If 10-gltf-viewer's folder is named differently — e.g. `damagedhelmet` or `DamagedHelmet` — match the actual name and update `demo.json`'s `gltfPath` to match. Verify with `ls games/10-gltf-viewer/assets`.)

- [ ] **Step 2: Author `games/11-sandbox/assets/scenes/demo.json`**

```json
{
  "clearColor": [0.45, 0.55, 0.7],
  "sun": { "direction": [-0.5, -1.0, -0.3], "color": [1.0, 0.97, 0.9], "ambient": 0.2 },
  "fog": { "color": [0.45, 0.55, 0.7], "density": 0.0 },
  "pointLights": [
    { "position": [-2.0, 3.0, 2.0], "color": [1.0, 0.6, 0.3], "intensity": 2.5, "range": 14.0 }
  ],
  "entities": [
    {
      "name": "floor",
      "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [20, 1, 20],
      "mesh": { "primitive": "plane" },
      "material": { "emissive": [0.05, 0.05, 0.06], "uvScale": 1.0 }
    },
    {
      "name": "cube-red",
      "position": [-2, 1, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1],
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.6, 0.15, 0.1] }
    },
    {
      "name": "cube-green",
      "position": [0, 1, -2], "rotation": [0, 0, 0, 1], "scale": [1, 2, 1],
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.1, 0.5, 0.15] }
    },
    {
      "name": "helmet",
      "position": [2.5, 1.5, 0], "rotation": [0, 0.3826834, 0, 0.9238795], "scale": [1.5, 1.5, 1.5],
      "mesh": { "gltfPath": "assets/damaged-helmet/DamagedHelmet.gltf" },
      "material": { "reflectivity": 0.2 }
    }
  ]
}
```

(The `gltfPath` must match the vendored file's actual name/case from Step 1.)

- [ ] **Step 3: Create `games/11-sandbox/CMakeLists.txt`**

Mirror `games/10-gltf-viewer/CMakeLists.txt` (read it first):

```cmake
if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(sandbox main.cpp)
    target_link_libraries(sandbox PRIVATE ironcore)

    add_custom_command(TARGET sandbox POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/assets
              $<TARGET_FILE_DIR:sandbox>/assets
      COMMENT "Copying sandbox assets")
endif()
```

- [ ] **Step 4: Register in root `CMakeLists.txt`**

After `add_subdirectory(games/10-gltf-viewer)` (~line 89), add:

```cmake
add_subdirectory(games/11-sandbox)
```

- [ ] **Step 5: Write `games/11-sandbox/main.cpp`**

Copy `games/10-gltf-viewer/main.cpp` as the starting point (keep its includes, the Vulkan `kVertexShader`/`kFragmentShader` lit shader strings, `FreeFlyCamera`, window/renderer setup, HUD). Then replace the "load one hardcoded model" section with scene-driven loading. Add `#include "scene/SceneIO.h"` and `#include "scene/SceneFormat.h"`.

The scene-resolution code (replaces the single-model load):

```cpp
// --- M29: load the scene file ---
const auto sceneOpt = iron::loadSceneFile("assets/scenes/demo.json");
if (!sceneOpt) {
    iron::Log::error("sandbox: failed to load assets/scenes/demo.json");
    return 1;
}
const iron::SceneFile& scene = *sceneOpt;

// One resolved, renderable entity: a mesh handle + a material with texture
// handles + a precomputed model matrix.
struct ResolvedEntity {
    iron::MeshHandle mesh = iron::kInvalidHandle;
    iron::Material   material;
    iron::Mat4       model = iron::Mat4::identity();
};
std::vector<ResolvedEntity> resolved;

// Cache primitive meshes so N cubes share one MeshHandle.
iron::MeshHandle cubeMesh  = iron::kInvalidHandle;
iron::MeshHandle planeMesh = iron::kInvalidHandle;
auto primitiveMesh = [&](iron::PrimitiveKind kind) -> iron::MeshHandle {
    if (kind == iron::PrimitiveKind::Cube) {
        if (cubeMesh == iron::kInvalidHandle) cubeMesh = renderer.createMesh(iron::makeCube());
        return cubeMesh;
    }
    if (planeMesh == iron::kInvalidHandle) {
        iron::MeshData q;
        iron::appendQuad(q, iron::Vec3{0, 0, 0}, iron::Vec2{1.0f, 1.0f}, iron::Vec3{0, 1, 0});
        planeMesh = renderer.createMesh(q);
    }
    return planeMesh;
};

auto resolveTexture = [&](const std::string& path, iron::TextureHandle fallback) {
    if (path.empty()) return fallback;
    iron::TextureHandle t = renderer.loadTexture(path);
    return (t == iron::kInvalidHandle) ? fallback : t;
};

for (const auto& e : scene.entities) {
    ResolvedEntity re;

    if (e.mesh.primitive.has_value()) {
        re.mesh = primitiveMesh(e.mesh.primitive.value());
    } else if (!e.mesh.gltfPath.empty()) {
        const auto model = iron::loadGltfModel(e.mesh.gltfPath);
        if (!model) {
            iron::Log::warn("sandbox: entity '%s' gltf '%s' failed to load; skipping",
                            e.name.c_str(), e.mesh.gltfPath.c_str());
            continue;
        }
        re.mesh = renderer.createMesh(model->mesh);
        // glTF brings its own material texture paths (M22.5).
        re.material.texture     = resolveTexture(model->materialPaths.albedo,        renderer.whiteTexture());
        re.material.normalMap   = resolveTexture(model->materialPaths.normal,        renderer.flatNormalTexture());
        re.material.specularMap = resolveTexture(model->materialPaths.metalRoughness, renderer.noSpecularTexture());
    } else {
        iron::Log::warn("sandbox: entity '%s' has no mesh; skipping", e.name.c_str());
        continue;
    }

    // Scene-file material overrides (textures only set here for primitives;
    // for glTF the gltf paths above win, then these scalar fields layer on).
    if (re.material.texture == iron::kInvalidHandle)
        re.material.texture = resolveTexture(e.material.albedoPath, renderer.whiteTexture());
    if (re.material.normalMap == iron::kInvalidHandle)
        re.material.normalMap = resolveTexture(e.material.normalPath, renderer.flatNormalTexture());
    if (re.material.specularMap == iron::kInvalidHandle)
        re.material.specularMap = resolveTexture(e.material.specularPath, renderer.noSpecularTexture());
    re.material.emissive     = e.material.emissive;
    re.material.uvScale      = e.material.uvScale;
    re.material.reflectivity = e.material.reflectivity;

    re.model = iron::translation(e.position)
             * e.rotation.toMat4()
             * iron::scaling(e.scale);
    resolved.push_back(re);
}

iron::Log::info("sandbox: resolved %zu entities from scene", resolved.size());
```

The per-frame render (replaces the viewer's single-model submit):

```cpp
renderer.beginFrame(scene.clearColor, scene.sun,
                    std::span<const iron::PointLight>(scene.pointLights.data(),
                                                      scene.pointLights.size()),
                    scene.fog, view, projection);
for (const auto& re : resolved) {
    iron::DrawCall call;
    call.mesh     = re.mesh;
    call.shader   = litShader;
    call.model    = re.model;
    call.material = re.material;
    renderer.submit(call);
}
renderer.endFrame();
```

`litShader`, `view`, `projection`, `renderer`, the HUD, and the free-fly camera all come from the copied 10-gltf-viewer scaffolding. Remove the viewer's `--model` CLI arg handling and its single-model state. Set `renderer.setShadowBounds(...)` to enclose the scene (e.g. center {0,2,0}, radius 25) and `renderer.disableReflectionPlane()` as the viewer does.

- [ ] **Step 6: Build**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target sandbox --config Debug
```

Expected: clean build; `build-vk/games/11-sandbox/Debug/assets/scenes/demo.json` + the helmet exist post-build.

- [ ] **Step 7: Visual check (human verifies; subagent confirms build)**

```powershell
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

Expected: a floor plane, two emissive cubes (red + tall green), and the Damaged Helmet under a warm point light; free-fly with WASD + mouse. The subagent only confirms the build + asset copy succeed; the visual confirmation is the user's.

- [ ] **Step 8: Commit**

```bash
git add games/11-sandbox/ CMakeLists.txt
git commit -m "M29 Task 3: sandbox runtime loads + renders a scene file"
```

---

## Task 4: Docs + PR

**Files:**
- Create: `docs/engine/scene-format.md`

- [ ] **Step 1: Write `docs/engine/scene-format.md`**

A reference doc for the scene JSON format. Cover:
- Purpose: the editor track's authored-level format; the data model the editor will read/write.
- The top-level keys (`clearColor`, `sun`, `fog`, `pointLights`, `entities`) with their shapes and defaults.
- `SceneEntity` fields (name, position, rotation [xyzw], scale, mesh, material).
- `MeshRef`: `primitive` ("cube" | "plane") OR `gltfPath`; primitive wins.
- `MaterialDef`: texture paths ("" = engine default), emissive, uvScale, reflectivity.
- The full `demo.json` as a worked example.
- Path resolution: relative to the runtime's working dir (where assets are copied).
- Limitations / v1 scope: static meshes only, no collision/audio/skybox/sphere yet (coming in later editor milestones).
- The C++ API: `loadSceneFile` / `saveSceneFile`, and that `SceneFile` is pure data (no GPU handles).

Match the prose style of `docs/engine/asset-pipeline.md`.

- [ ] **Step 2: Commit, push, open PR**

```bash
git add docs/engine/scene-format.md
git commit -m "M29: document the scene file format"
git push -u origin feat/m29-scene-serialization
```

Open the PR matching the M28 (#48) template style. Title: `M29: Scene serialization + sandbox runtime`. Body:

```
## Summary

- Added `iron::SceneFile` — a serializable scene model (entities reference meshes/textures by path or primitive name, never by GPU handle).
- Added `iron::loadSceneFile` / `saveSceneFile` — JSON load/save via vendored nlohmann/json. Pure data, headlessly unit-tested (round-trip + malformed + defaults).
- Added `games/11-sandbox` — loads `assets/scenes/demo.json` and renders it (primitives + a static glTF) under the scene's lighting with a free-fly camera.
- First milestone of the editor track: this is the data foundation the editor UI will author in later milestones.

## Test plan

- [x] `test_scene_io` — round-trip preserves all fields; malformed JSON → nullopt; missing file → nullopt; minimal scene uses defaults (4 cases)
- [x] Full suite green (43/43)
- [x] sandbox builds clean
- [ ] Visual: sandbox renders demo.json (floor + 2 cubes + helmet) with free-fly camera

## Known v1 limitations

- Static meshes only (no skinned/animated entities in scenes yet).
- No collision shapes, audio emitters, skybox, or sphere primitive — these arrive as the editor gains the UI to author them.
- Scene asset paths resolve relative to the runtime's working directory.
- No live scene hot-reload (relaunch to see edits).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — 43/43 green (42 from M28 + `test_scene_io`).
- [ ] `sandbox` builds clean; assets copy next to the exe.
- [ ] Visual: `sandbox.exe` renders the floor + 2 emissive cubes + the Damaged Helmet under a point light; free-fly works.
- [ ] Edit `demo.json` (move an entity), relaunch → change reflected.
- [ ] PR CI green.
