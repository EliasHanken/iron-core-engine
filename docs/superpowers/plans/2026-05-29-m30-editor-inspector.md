# M30 — Editor Module + Scene Inspector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up an `ironcore_editor` module (Dear ImGui integrated into the Vulkan renderer) and ship a scene inspector in `games/11-sandbox` that lets you select an entity, edit its transform/material and the scene's lighting live, and save back to the scene file.

**Architecture:** A new Vulkan-gated static lib `ironcore_editor` (`engine/editor/`) holds `ImGuiLayer` (the ImGui↔Vulkan↔GLFW integration, driven entirely through the renderer's existing `context()` / `scenePass()` / `enqueueDeferredScenePass()` accessors — no renderer changes) plus three panels (`SceneOutliner`, `SceneInspector`, `EnvironmentPanel`) that operate on a `SceneFile&`. The sandbox becomes the editor host: it keeps a mutable `SceneFile`, drives a hold-RMB-to-look camera, and re-derives its render data from the (possibly edited) scene each frame.

**Tech Stack:** C++23 (`/std:c++latest`), Vulkan 1.3, Dear ImGui via vcpkg (`imgui[glfw-binding,vulkan-binding]`), CMake, CTest.

**Spec:** `docs/superpowers/specs/2026-05-29-m30-editor-inspector-design.md`

**Branch:** `feat/m30-editor-inspector` (cut off `main` at the M29 merge, spec committed).

---

## Ground-truth facts (verified — match exactly)

```cpp
// engine/render/backends/vulkan/VkContext.h — ALL ImGui init inputs already exist:
VkInstance instance(); VkPhysicalDevice physicalDevice(); VkDevice device();
VkQueue graphicsQueue(); std::uint32_t graphicsFamily();
// engine/render/backends/vulkan/VulkanRenderer.h — engine-internal accessors:
VkContext& context();                  VkRenderPass scenePass() const;
VkCommandBuffer currentCommandBuffer(); void enqueueDeferredScenePass(std::function<void(VkCommandBuffer)> fn);
//   (enqueueDeferredScenePass fires inside the scene pass during endFrame, before debug-lines+HUD)
// engine/core/Window.h:
GLFWwindow* handle() const;  void setCursorCaptured(bool);  int width()/height();
// engine/math/Quaternion.h:  struct Quat { float x,y,z,w; static Quat identity(); Mat4 toMat4(); };
// engine/math/Vec.h:  struct Vec3 { float x,y,z; };  (x,y,z contiguous — &v.x is a float[3])
// engine/scene/SceneFormat.h:  SceneFile{entities,sun,pointLights,fog,clearColor},
//   SceneEntity{name,position,rotation,scale,mesh,material}, MeshRef{optional<PrimitiveKind>primitive,gltfPath},
//   MaterialDef{albedoPath,normalPath,specularPath,emissive,uvScale,reflectivity}, PrimitiveKind{Cube,Plane}
// engine/render/Light.h: DirectionalLight{direction,color,ambient}; PointLight{position,color,intensity,range}
// engine/render/Fog.h: Fog{color,density}
// tests/CMakeLists.txt: iron_add_test(name source.cpp)  — 2-arg macro
// iron::createRenderer always returns a VulkanRenderer in a Vulkan build.
```

**ImGui version note (read once before Task 1):** the code below targets Dear ImGui ≥ 1.90 (vcpkg ships recent). Two APIs shifted historically — if the installed `imgui` is older, adjust *only* these:
- `ImGui_ImplVulkan_Init(&initInfo)` (1-arg, with `initInfo.RenderPass` set). Pre-1.91 form is `ImGui_ImplVulkan_Init(&initInfo, renderPass)` (2-arg, no `.RenderPass` field).
- `ImGui_ImplVulkan_CreateFontsTexture()` (no-arg, ≥1.90). Pre-1.90 needs a one-shot command buffer + `ImGui_ImplVulkan_DestroyFontUploadObjects()`.
Confirm the version via `vcpkg list imgui` or the installed `imgui.h` `IMGUI_VERSION` and use the matching form.

---

## File Structure

**Create:**
- `engine/editor/CMakeLists.txt` — `ironcore_editor` static lib.
- `engine/editor/ImGuiLayer.h` / `.cpp` — ImGui↔Vulkan↔GLFW integration.
- `engine/editor/SceneOutliner.h` / `.cpp` — entity list + Save button.
- `engine/editor/SceneInspector.h` / `.cpp` — selected-entity transform/material editor.
- `engine/editor/EnvironmentPanel.h` / `.cpp` — sun/fog/clearColor/point-light editor.
- `tests/test_quat_euler.cpp` — euler round-trip test.
- `docs/engine/editor.md` — editor doc.

**Modify:**
- `vcpkg.json` — add `imgui` with bindings.
- `CMakeLists.txt` (root) — `add_subdirectory(engine/editor)` (Vulkan-gated).
- `engine/math/Quaternion.h` — `quatToEuler` / `eulerToQuat` (inline).
- `tests/CMakeLists.txt` — register `test_quat_euler`.
- `games/11-sandbox/main.cpp` — host the editor (mutable scene, RMB camera, panels, drop `iron::Hud`).
- `games/11-sandbox/CMakeLists.txt` — link `ironcore_editor`.

---

## Task 1: ImGui dependency + `ironcore_editor` module + `ImGuiLayer`

**Files:**
- Modify: `vcpkg.json`
- Create: `engine/editor/CMakeLists.txt`, `engine/editor/ImGuiLayer.h`, `engine/editor/ImGuiLayer.cpp`
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Add ImGui to `vcpkg.json`**

Replace the dependencies array so `imgui` (with both bindings) is included:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "joltphysics",
    "openal-soft",
    "tinygltf",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator",
    { "name": "imgui", "features": ["glfw-binding", "vulkan-binding"] }
  ]
}
```

- [ ] **Step 2: Create `engine/editor/ImGuiLayer.h`**

```cpp
#pragma once

namespace iron {

class Window;
class Renderer;

// Integrates Dear ImGui with the engine's Vulkan renderer and GLFW window.
// The editor module is Vulkan-only; init() expects the renderer to be the
// VulkanRenderer (always true in a Vulkan build). ImGui records into the
// scene render pass via the renderer's deferred-overlay hook, so no separate
// render pass or renderer change is needed.
//
// Usage per frame:
//   layer.beginFrame();       // before building any ImGui windows
//   ...build ImGui windows... // (panels)
//   layer.render();           // after windows; enqueues the UI as an overlay
//   renderer.endFrame();      // draws the enqueued UI on top of the scene
class ImGuiLayer {
public:
    bool init(Window& window, Renderer& renderer);
    void beginFrame();
    void render();
    void shutdown();

    // True while ImGui owns the pointer / keyboard (hovering or editing a
    // widget). The host suppresses camera/game input when these are true.
    bool wantsMouse() const;
    bool wantsKeyboard() const;

private:
    bool initialized_ = false;
    void* device_ = nullptr;          // VkDevice, stored opaquely for shutdown
    void* descriptorPool_ = nullptr;  // VkDescriptorPool, owned by this layer
    Renderer* renderer_ = nullptr;
};

}  // namespace iron
```

(Storing `VkDevice`/`VkDescriptorPool` as `void*` keeps `<vulkan/vulkan.h>` out of the public header; the `.cpp` casts them back. `renderer_` is the abstract `Renderer*`; the `.cpp` casts to `VulkanRenderer` for the engine-internal accessors.)

- [ ] **Step 3: Create `engine/editor/ImGuiLayer.cpp`**

```cpp
#include "editor/ImGuiLayer.h"

#include "core/Log.h"
#include "core/Window.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <vulkan/vulkan.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

namespace iron {

bool ImGuiLayer::init(Window& window, Renderer& renderer) {
    renderer_ = &renderer;
    auto& vk = static_cast<VulkanRenderer&>(renderer);  // editor is Vulkan-only
    VkContext& ctx = vk.context();
    const VkDevice device = ctx.device();
    device_ = device;

    // ImGui needs its own descriptor pool sized for combined image samplers.
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 1000;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        Log::error("ImGuiLayer: vkCreateDescriptorPool failed");
        return false;
    }
    descriptorPool_ = pool;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // install_callbacks=true is safe: iron::Input is poll-based (it never
    // installs GLFW callbacks), and ImGui chains to any pre-existing ones.
    ImGui_ImplGlfw_InitForVulkan(window.handle(), /*install_callbacks=*/true);

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance       = ctx.instance();
    info.PhysicalDevice = ctx.physicalDevice();
    info.Device         = device;
    info.QueueFamily    = ctx.graphicsFamily();
    info.Queue          = ctx.graphicsQueue();
    info.DescriptorPool = pool;
    info.MinImageCount  = 2;
    info.ImageCount     = 2;   // matches the engine's 2 frames in flight
    info.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
    info.RenderPass     = vk.scenePass();   // ImGui draws into the scene pass
    ImGui_ImplVulkan_Init(&info);           // 1-arg form (ImGui >= 1.91)

    ImGui_ImplVulkan_CreateFontsTexture();  // no-arg form (ImGui >= 1.90)

    initialized_ = true;
    Log::info("ImGuiLayer: initialized");
    return true;
}

void ImGuiLayer::beginFrame() {
    if (!initialized_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    if (!initialized_) return;
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    // The deferred callback fires inside the scene pass during endFrame() of
    // THIS frame, before the next NewFrame(), so drawData stays valid.
    auto& vk = static_cast<VulkanRenderer&>(*renderer_);
    vk.enqueueDeferredScenePass([drawData](VkCommandBuffer cb) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    });
}

void ImGuiLayer::shutdown() {
    if (!initialized_) return;
    const VkDevice device = static_cast<VkDevice>(device_);
    vkDeviceWaitIdle(device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(descriptorPool_), nullptr);
    initialized_ = false;
}

bool ImGuiLayer::wantsMouse() const {
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const {
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace iron
```

If the installed ImGui is older than noted, adjust only the two flagged calls (`ImGui_ImplVulkan_Init` arity + `CreateFontsTexture`) per the version note at the top of this plan.

- [ ] **Step 4: Create `engine/editor/CMakeLists.txt`**

```cmake
# ironcore_editor — the engine's editor layer. Unreal-style: the editor is its
# own module layered on the runtime; shipping games never link it (so they never
# pull in ImGui). Vulkan-only (added from the root only under the Vulkan backend).
find_package(imgui CONFIG REQUIRED)

add_library(ironcore_editor STATIC
  ImGuiLayer.cpp
  SceneOutliner.cpp
  SceneInspector.cpp
  EnvironmentPanel.cpp
)

# ironcore PUBLIC propagates the engine include dir (engine/), GLFW, Vulkan, and
# the IRON_RENDER_BACKEND_VULKAN define. ImGui stays PRIVATE — no ImGui type
# appears in this module's public headers.
target_link_libraries(ironcore_editor PUBLIC ironcore)
target_link_libraries(ironcore_editor PRIVATE imgui::imgui)
```

**NOTE for Steps 5+ (the panels):** `SceneOutliner.cpp`, `SceneInspector.cpp`, and `EnvironmentPanel.cpp` are listed here but created in Task 3. CMake refuses to configure if a listed source file is missing. So in THIS task, create empty stub `.cpp` files for the three panels (each just `#include` of a one-line header you also stub, or simply an empty TU: `// stub - implemented in Task 3`). The stubs let `ironcore_editor` configure + compile now; Task 3 fills them in. (Same TDD-vs-CMake accommodation used in M28/M29.)

Create these stubs now:
- `engine/editor/SceneOutliner.cpp` → `// stub - implemented in M30 Task 3` 
- `engine/editor/SceneInspector.cpp` → `// stub - implemented in M30 Task 3`
- `engine/editor/EnvironmentPanel.cpp` → `// stub - implemented in M30 Task 3`

- [ ] **Step 5: Register the editor subdir in root `CMakeLists.txt`**

Immediately after the `add_subdirectory(engine)` line (currently line 79), add:

```cmake

# Editor module (Unreal-style): only built under Vulkan; only editor hosts link it.
if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    add_subdirectory(engine/editor)
endif()
```

- [ ] **Step 6: Configure + build the editor lib**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target ironcore_editor --config Debug
```

Expected: vcpkg resolves `imgui` (first configure may take longer while it builds imgui), `ironcore_editor` compiles + links (ImGuiLayer.cpp + the three stub TUs). If `<imgui_impl_glfw.h>` / `<imgui_impl_vulkan.h>` are not found, confirm the vcpkg install path for the backend headers and adjust the includes (vcpkg installs them into the include root; some layouts use `<backends/...>`). If `ImGui_ImplVulkan_Init` arity errors, apply the version note.

- [ ] **Step 7: Commit**

```powershell
git add vcpkg.json engine/editor/ CMakeLists.txt
git commit -m "M30 Task 1: ironcore_editor module + ImGuiLayer (ImGui Vulkan integration)"
```

---

## Task 2: quat↔euler math helpers (TDD)

**Files:**
- Create: `tests/test_quat_euler.cpp`
- Modify: `tests/CMakeLists.txt`, `engine/math/Quaternion.h`

- [ ] **Step 1: Write the failing test `tests/test_quat_euler.cpp`**

Match the harness shape of `tests/test_quaternion.cpp` (includes `test_framework.h`, `int main()` with `CHECK` / `CHECK_NEAR`, ends `return iron_test_result();`). `CHECK_NEAR` is a 2-arg macro here (tolerance hardcoded ~1e-4f).

```cpp
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

int main() {
    // eulerToQuat(0,0,0) is identity.
    {
        const Quat q = eulerToQuat(Vec3{0.0f, 0.0f, 0.0f});
        CHECK_NEAR(q.x, 0.0f);
        CHECK_NEAR(q.y, 0.0f);
        CHECK_NEAR(q.z, 0.0f);
        CHECK_NEAR(q.w, 1.0f);
    }

    // Round-trip: euler -> quat -> euler returns the same angles (degrees),
    // for angles safely away from gimbal-lock (|pitch| != 90).
    {
        const Vec3 e{30.0f, 45.0f, -60.0f};
        const Quat q = eulerToQuat(e);
        const Vec3 r = quatToEuler(q);
        CHECK_NEAR(r.x, e.x);
        CHECK_NEAR(r.y, e.y);
        CHECK_NEAR(r.z, e.z);
    }

    // A known single-axis rotation: 90 deg about Y == fromAxisAngle(Y, pi/2).
    {
        const Quat q = eulerToQuat(Vec3{0.0f, 90.0f, 0.0f});
        const Quat e = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5707963f);
        CHECK_NEAR(q.x, e.x);
        CHECK_NEAR(q.y, e.y);
        CHECK_NEAR(q.z, e.z);
        CHECK_NEAR(q.w, e.w);
    }

    // quatToEuler(identity) is zero.
    {
        const Vec3 r = quatToEuler(Quat::identity());
        CHECK_NEAR(r.x, 0.0f);
        CHECK_NEAR(r.y, 0.0f);
        CHECK_NEAR(r.z, 0.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Register the test in `tests/CMakeLists.txt`**

After the last `iron_add_test(...)` line (currently `test_scene_io`), add:

```cmake
iron_add_test(test_quat_euler test_quat_euler.cpp)
```

- [ ] **Step 3: Run to verify it fails (red)**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target test_quat_euler --config Debug
```

Expected: compile/link error — `eulerToQuat` / `quatToEuler` are undeclared.

- [ ] **Step 4: Implement the helpers in `engine/math/Quaternion.h`**

Add these free functions just before the closing `} // namespace iron` (after the `slerp` definition). They use the standard intrinsic Tait–Bryan XYZ convention (rotate about X, then Y, then Z), angles in **degrees** to match editor UI.

```cpp
// --- Euler (degrees, intrinsic XYZ) <-> quaternion, for editor rotation UI ---
// Not for animation/runtime math (use quaternions directly); these exist so a
// Details-panel can show/edit rotation as human-readable degrees.

inline Quat eulerToQuat(Vec3 degrees) {
    const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    const float hx = degrees.x * kDeg2Rad * 0.5f;
    const float hy = degrees.y * kDeg2Rad * 0.5f;
    const float hz = degrees.z * kDeg2Rad * 0.5f;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);
    // q = qz * qy * qx  (apply X first, then Y, then Z)
    Quat q;
    q.w = cx * cy * cz + sx * sy * sz;
    q.x = sx * cy * cz - cx * sy * sz;
    q.y = cx * sy * cz + sx * cy * sz;
    q.z = cx * cy * sz - sx * sy * cz;
    return q.normalized();
}

inline Vec3 quatToEuler(Quat q) {
    q = q.normalized();
    const float kRad2Deg = 180.0f / 3.14159265358979323846f;
    // X (roll about model X)
    const float sinrCosp = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosrCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float x = std::atan2(sinrCosp, cosrCosp);
    // Y (pitch) — clamp for gimbal-lock safety
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    sinp = sinp > 1.0f ? 1.0f : (sinp < -1.0f ? -1.0f : sinp);
    const float y = std::asin(sinp);
    // Z (yaw)
    const float sinyCosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosyCosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float z = std::atan2(sinyCosp, cosyCosp);
    return Vec3{x * kRad2Deg, y * kRad2Deg, z * kRad2Deg};
}
```

(`Quat::normalized()` and `Vec3` are already in scope from this header / `math/Vec.h`. `<cmath>` is already included at the top of `Quaternion.h`.)

- [ ] **Step 5: Run to verify it passes (green)**

```powershell
cmake --build build-vk --target test_quat_euler --config Debug
ctest --test-dir build-vk -C Debug -R quat_euler --output-on-failure
```

Expected: all 4 blocks pass.

- [ ] **Step 6: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: previous green count + 1 (M29 left it at 43; now 44).

- [ ] **Step 7: Commit**

```powershell
git add engine/math/Quaternion.h tests/test_quat_euler.cpp tests/CMakeLists.txt
git commit -m "M30 Task 2: quat<->euler helpers (degrees, XYZ) with round-trip test"
```

---

## Task 3: Editor panels (Outliner, Inspector, Environment)

**Files:**
- Create: `engine/editor/SceneOutliner.h`, `engine/editor/SceneInspector.h`, `engine/editor/EnvironmentPanel.h`
- Replace stubs: `engine/editor/SceneOutliner.cpp`, `engine/editor/SceneInspector.cpp`, `engine/editor/EnvironmentPanel.cpp`

These have no headless test (pure ImGui UI). Verification is a clean compile here + the visual check in Task 4.

- [ ] **Step 1: `engine/editor/SceneOutliner.h`**

```cpp
#pragma once

namespace iron {

struct SceneFile;

// Lists the scene's entities and a Save button. Pure UI: it mutates the
// selection index and reports whether Save was clicked; it does not own the
// scene or the file path (the host performs the save).
class SceneOutliner {
public:
    // Returns true if the user clicked "Save Scene" this frame.
    // `selectedIndex` is updated in place when the user clicks an entity.
    bool draw(const SceneFile& scene, int& selectedIndex);
};

}  // namespace iron
```

- [ ] **Step 2: `engine/editor/SceneOutliner.cpp`**

```cpp
#include "editor/SceneOutliner.h"

#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    bool saveClicked = false;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) saveClicked = true;
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();
        ImGui::PushID(i);  // unique id even when names collide
        if (ImGui::Selectable(label, i == selectedIndex)) selectedIndex = i;
        ImGui::PopID();
    }

    ImGui::End();
    return saveClicked;
}

}  // namespace iron
```

- [ ] **Step 3: `engine/editor/SceneInspector.h`**

```cpp
#pragma once

namespace iron {

struct SceneEntity;

// Details panel for a single entity: transform (position, euler rotation,
// scale) and material scalars (emissive, uvScale, reflectivity). Mesh info is
// shown read-only. Mutates the entity in place.
class SceneInspector {
public:
    // Returns true if any field changed this frame.
    bool draw(SceneEntity& entity);
};

}  // namespace iron
```

- [ ] **Step 4: `engine/editor/SceneInspector.cpp`**

```cpp
#include "editor/SceneInspector.h"

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool SceneInspector::draw(SceneEntity& e) {
    bool changed = false;
    ImGui::Begin("Inspector");

    ImGui::Text("Name: %s", e.name.empty() ? "(unnamed)" : e.name.c_str());

    ImGui::SeparatorText("Transform");
    changed |= ImGui::DragFloat3("Position", &e.position.x, 0.05f);

    Vec3 euler = quatToEuler(e.rotation);  // degrees
    if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
        e.rotation = eulerToQuat(euler);
        changed = true;
    }

    changed |= ImGui::DragFloat3("Scale", &e.scale.x, 0.05f);

    ImGui::SeparatorText("Material");
    changed |= ImGui::ColorEdit3("Emissive", &e.material.emissive.x);
    changed |= ImGui::DragFloat("UV Scale", &e.material.uvScale, 0.05f, 0.0f, 64.0f);
    changed |= ImGui::SliderFloat("Reflectivity", &e.material.reflectivity, 0.0f, 1.0f);

    ImGui::SeparatorText("Mesh (read-only)");
    if (e.mesh.primitive.has_value()) {
        ImGui::Text("primitive: %s",
                    e.mesh.primitive.value() == PrimitiveKind::Cube ? "cube" : "plane");
    } else {
        ImGui::Text("gltf: %s", e.mesh.gltfPath.c_str());
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
```

(`&e.position.x` / `&e.scale.x` / `&e.material.emissive.x` are valid `float[3]` because `Vec3` is `{x,y,z}` contiguous. `SeparatorText` is ImGui ≥ 1.89; if older, replace each with `ImGui::Separator(); ImGui::Text("...");`.)

- [ ] **Step 5: `engine/editor/EnvironmentPanel.h`**

```cpp
#pragma once

namespace iron {

struct SceneFile;

// Edits the scene's global lighting + environment: clear color, sun, fog, and
// the point-light list. Mutates the scene in place.
class EnvironmentPanel {
public:
    // Returns true if any field changed this frame.
    bool draw(SceneFile& scene);
};

}  // namespace iron
```

- [ ] **Step 6: `engine/editor/EnvironmentPanel.cpp`**

```cpp
#include "editor/EnvironmentPanel.h"

#include "render/Fog.h"
#include "render/Light.h"
#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

bool EnvironmentPanel::draw(SceneFile& scene) {
    bool changed = false;
    ImGui::Begin("Environment");

    changed |= ImGui::ColorEdit3("Clear Color", &scene.clearColor.x);

    ImGui::SeparatorText("Sun");
    changed |= ImGui::DragFloat3("Direction", &scene.sun.direction.x, 0.02f, -1.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Sun Color", &scene.sun.color.x);
    changed |= ImGui::SliderFloat("Ambient", &scene.sun.ambient, 0.0f, 1.0f);

    ImGui::SeparatorText("Fog");
    changed |= ImGui::ColorEdit3("Fog Color", &scene.fog.color.x);
    changed |= ImGui::DragFloat("Fog Density", &scene.fog.density, 0.001f, 0.0f, 1.0f);

    ImGui::SeparatorText("Point Lights");
    for (int i = 0; i < static_cast<int>(scene.pointLights.size()); ++i) {
        ImGui::PushID(i);
        PointLight& pl = scene.pointLights[i];
        ImGui::Text("Light %d", i);
        changed |= ImGui::DragFloat3("Position", &pl.position.x, 0.05f);
        changed |= ImGui::ColorEdit3("Color", &pl.color.x);
        changed |= ImGui::DragFloat("Intensity", &pl.intensity, 0.05f, 0.0f, 50.0f);
        changed |= ImGui::DragFloat("Range", &pl.range, 0.1f, 0.0f, 200.0f);
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
```

- [ ] **Step 7: Build the editor lib**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target ironcore_editor --config Debug
```

Expected: clean compile of all four editor TUs (ImGuiLayer + 3 real panels).

- [ ] **Step 8: Commit**

```powershell
git add engine/editor/SceneOutliner.* engine/editor/SceneInspector.* engine/editor/EnvironmentPanel.*
git commit -m "M30 Task 3: editor panels (Outliner, Inspector, Environment) over SceneFile"
```

---

## Task 4: Sandbox editor host integration

**Files:**
- Modify: `games/11-sandbox/main.cpp`, `games/11-sandbox/CMakeLists.txt`

Turn the sandbox into the editor host: mutable scene, hold-RMB-to-look camera, the three panels + `ImGuiLayer`, per-frame re-derive of render data, drop the old `iron::Hud`.

- [ ] **Step 1: Link `ironcore_editor` in `games/11-sandbox/CMakeLists.txt`**

Read the file; it links `ironcore`. Add `ironcore_editor` to the same `target_link_libraries(sandbox PRIVATE ...)` call:

```cmake
    target_link_libraries(sandbox PRIVATE ironcore ironcore_editor)
```

(The sandbox is already inside the `if (IRON_RENDER_BACKEND STREQUAL "vulkan")` guard, matching where `ironcore_editor` exists.)

- [ ] **Step 2: Update includes in `games/11-sandbox/main.cpp`**

Add the editor headers; remove the now-unused HUD/font headers. Change the include block so it has the editor includes and DROPS `ui/BuiltinFont.h` + `ui/Hud.h`:

Add:
```cpp
#include "editor/EnvironmentPanel.h"
#include "editor/ImGuiLayer.h"
#include "editor/SceneInspector.h"
#include "editor/SceneOutliner.h"
```
Remove:
```cpp
#include "ui/BuiltinFont.h"
#include "ui/Hud.h"
```

- [ ] **Step 3: Make the scene mutable + give `ResolvedEntity` a source index**

Change the scene binding from a const reference to an owned mutable copy:

Find:
```cpp
    const iron::SceneFile& scene = *sceneOpt;
```
Replace with:
```cpp
    iron::SceneFile scene = *sceneOpt;  // mutable: the editor edits this in place
```

Change the `ResolvedEntity` struct to remember which entity it came from:

Find:
```cpp
    struct ResolvedEntity {
        iron::MeshHandle mesh     = iron::kInvalidHandle;
        iron::Material   material;
        iron::Mat4       model    = iron::Mat4::identity();
    };
```
Replace with:
```cpp
    struct ResolvedEntity {
        int              entityIndex = -1;  // index into scene.entities
        iron::MeshHandle mesh     = iron::kInvalidHandle;
        iron::Material   material;
        iron::Mat4       model    = iron::Mat4::identity();
    };
```

- [ ] **Step 4: Record the source index in the resolve loop**

The resolve loop currently iterates `for (const auto& e : scene.entities)`. Convert it to an indexed loop and stamp `re.entityIndex`. Change the loop header:

Find:
```cpp
    for (const auto& e : scene.entities) {
        ResolvedEntity re;
```
Replace with:
```cpp
    for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {
        const iron::SceneEntity& e = scene.entities[ei];
        ResolvedEntity re;
        re.entityIndex = ei;
```

(The rest of the loop body — primitive/glTF resolution, material setup, `re.model = ...`, `resolved.push_back(re)` — is unchanged. Entities that fail to load still `continue` and simply have no `resolved` entry; their index gap is harmless.)

- [ ] **Step 5: Remove the HUD setup block**

Delete the entire HUD setup (the font atlas, font texture, `iron::Hud hud;`, and the two `hud.addText(...)` calls). Find and DELETE:

```cpp
    // --- HUD ---
    const iron::BuiltinFontAtlas fontAtlas = iron::builtinFontAtlas();
    const iron::TextureHandle fontTexture =
        renderer.createTexture(fontAtlas.width, fontAtlas.height,
                               fontAtlas.rgba.data());
    const iron::BitmapFont font = iron::builtinFont(fontTexture);

    iron::Hud hud;
    char hudBuf[160];
    std::snprintf(hudBuf, sizeof(hudBuf),
                  "Entities: %zu  |  demo.json", resolved.size());
    hud.addText(hudBuf, iron::Vec2{10, 10}, 2.0f,
                iron::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    hud.addText("WASD: move  mouse: look  Space/Ctrl: up/down  ESC: quit",
                iron::Vec2{10, static_cast<float>(kScreenH - 28)}, 1.5f,
                iron::Vec4{1.0f, 1.0f, 0.0f, 1.0f});
```

- [ ] **Step 6: Initialize the editor + panels, and switch cursor to free-by-default**

The current code calls `app.window().setCursorCaptured(true);`. Change that to `false` (cursor free so the UI is usable by default) and, just before the `// --- Main loop ---` comment, construct the editor objects + selection + look-state:

Find:
```cpp
    app.window().setCursorCaptured(true);
```
Replace with:
```cpp
    app.window().setCursorCaptured(false);  // free by default; RMB captures look

    // --- Editor ---
    iron::ImGuiLayer imgui;
    if (!imgui.init(app.window(), renderer)) {
        iron::Log::error("sandbox: ImGui init failed");
        return 1;
    }
    iron::SceneOutliner   outliner;
    iron::SceneInspector  inspector;
    iron::EnvironmentPanel environment;
    int  selectedIndex = scene.entities.empty() ? -1 : 0;
    bool prevLook = false;  // was the camera capturing last frame?
```

- [ ] **Step 7: Replace the update lambda with the hold-RMB-to-look model**

Find the whole `app.setUpdate([&](const iron::FrameTime& t) { ... });` block and replace it with:

```cpp
    app.setUpdate([&](const iron::FrameTime& t) {
        iron::Input& input = app.input();
        if (input.keyPressed(GLFW_KEY_ESCAPE))
            glfwSetWindowShouldClose(app.window().handle(), GLFW_TRUE);

        // Look + fly only while RIGHT mouse is held and ImGui isn't using it.
        const bool look = input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                          && !imgui.wantsMouse();
        app.window().setCursorCaptured(look);

        const float mdx = static_cast<float>(input.mouseDeltaX());
        const float mdy = static_cast<float>(input.mouseDeltaY());

        if (look && prevLook) {
            cam.update(t.deltaSeconds, mdx, mdy,
                       input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                       input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                       input.keyDown(GLFW_KEY_LEFT_CONTROL),
                       input.keyDown(GLFW_KEY_SPACE),
                       3.0f);
        } else if (look && !prevLook) {
            // First capture frame: move but ignore the (stale) mouse delta so
            // the camera doesn't snap when the cursor recenters.
            cam.update(t.deltaSeconds, 0.0f, 0.0f,
                       input.keyDown(GLFW_KEY_W), input.keyDown(GLFW_KEY_S),
                       input.keyDown(GLFW_KEY_A), input.keyDown(GLFW_KEY_D),
                       input.keyDown(GLFW_KEY_LEFT_CONTROL),
                       input.keyDown(GLFW_KEY_SPACE),
                       3.0f);
        }
        prevLook = look;
    });
```

- [ ] **Step 8: Replace the render lambda to drive the panels + re-derive + draw ImGui**

Find the whole `app.setRender([&]() { ... });` block and replace it with:

```cpp
    app.setRender([&]() {
        // --- editor UI ---
        imgui.beginFrame();
        const bool saveClicked = outliner.draw(scene, selectedIndex);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            inspector.draw(scene.entities[selectedIndex]);
        environment.draw(scene);
        if (saveClicked) {
            if (iron::saveSceneFile(scene, scenePath))
                iron::Log::info("sandbox: saved %s", scenePath.c_str());
            else
                iron::Log::error("sandbox: save FAILED for %s", scenePath.c_str());
        }

        // --- re-derive render data from the (possibly edited) scene ---
        // Mesh + texture handles are fixed (path editing is out of scope), so
        // only the model matrix + material scalars need refreshing. Lighting is
        // read live by beginFrame below.
        for (auto& re : resolved) {
            const iron::SceneEntity& e = scene.entities[re.entityIndex];
            re.model = iron::translation(e.position)
                     * e.rotation.toMat4()
                     * iron::scaling(e.scale);
            re.material.emissive     = e.material.emissive;
            re.material.uvScale      = e.material.uvScale;
            re.material.reflectivity = e.material.reflectivity;
        }

        // --- scene render ---
        const iron::Mat4 view = cam.viewMatrix();
        renderer.beginFrame(scene.clearColor, scene.sun,
                            std::span<const iron::PointLight>(
                                scene.pointLights.data(),
                                scene.pointLights.size()),
                            scene.fog, view, proj);
        for (const auto& re : resolved) {
            iron::DrawCall call;
            call.mesh     = re.mesh;
            call.shader   = litShader;
            call.model    = re.model;
            call.material = re.material;
            renderer.submit(call);
        }
        imgui.render();   // enqueues the UI overlay into the scene pass tail
        renderer.endFrame();
        app.window().swapBuffers();
    });
```

- [ ] **Step 9: Shut down ImGui after the loop**

Find:
```cpp
    app.run();
    return 0;
```
Replace with:
```cpp
    app.run();
    imgui.shutdown();   // before renderer/context teardown
    return 0;
```

- [ ] **Step 10: Build**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target sandbox --config Debug
```

Expected: clean build of `sandbox` linking `ironcore_editor`. The `<cstdio>` / `<numbers>` includes already present stay; the `std::snprintf` HUD usage is gone so no new warnings. If unused-include warnings appear for removed HUD headers, ensure Step 2's removals were applied.

- [ ] **Step 11: Visual check (human verifies; subagent confirms build only)**

```powershell
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

Expected (user-verified): three ImGui windows (Scene Outliner, Inspector, Environment). Clicking an entity selects it; dragging Position/Rotation/Scale or material fields updates the viewport live; editing sun/fog/clearColor/point-light re-lights live; **hold RMB** to fly with WASD + mouse, release to click UI; **Save Scene** writes `demo.json`. The subagent only confirms the build + link succeed.

- [ ] **Step 12: Commit**

```powershell
git add games/11-sandbox/main.cpp games/11-sandbox/CMakeLists.txt
git commit -m "M30 Task 4: sandbox becomes the editor host (inspector + RMB camera + save)"
```

---

## Task 5: Docs + PR

**Files:**
- Create: `docs/engine/editor.md`

- [ ] **Step 1: Write `docs/engine/editor.md`**

Match the prose style of `docs/engine/scene-format.md`. Cover:
- Purpose: the editor track's first UI; an Unreal-style editor module (`ironcore_editor`) layered on the runtime — only editor *hosts* link it, shipping games don't.
- Module layout: `ImGuiLayer` (ImGui↔Vulkan↔GLFW integration via the renderer's `context()`/`scenePass()`/`enqueueDeferredScenePass()` hooks; no renderer changes), and the three panels (`SceneOutliner`, `SceneInspector`, `EnvironmentPanel`) operating on a `SceneFile&`.
- The host contract: keep a mutable `SceneFile`, call `imgui.beginFrame()` → draw panels → re-derive render data → `beginFrame`/submit → `imgui.render()` → `endFrame`; `imgui.shutdown()` after the loop.
- Interaction: hold-RMB-to-look (cursor free by default for the UI); `wantsMouse()`/`wantsKeyboard()` gate game input.
- Editing scope: select entity → transform (rotation edited as euler degrees, stored as quaternion via `quatToEuler`/`eulerToQuat`) + material scalars; global sun/fog/clearColor/point lights; **Save** via `saveSceneFile`.
- How to run the sandbox editor + the dependency (`imgui` via vcpkg with glfw/vulkan bindings).
- Limitations (v1): no add/delete/mesh-swap/gizmos (→ M31); single selection; save overwrites in place; no undo; Vulkan-only.

- [ ] **Step 2: Commit, push, open PR**

```powershell
git add docs/engine/editor.md
git commit -m "M30: document the editor module + scene inspector"
git push -u origin feat/m30-editor-inspector
```

Open the PR (match the M29 #49 template). Title: `M30: Editor module + scene inspector`. Body:

```
## Summary

- Added `ironcore_editor` — the engine's editor module (Unreal-style: layered on the runtime, only editor hosts link it; shipping games never pull in ImGui).
- `ImGuiLayer` integrates Dear ImGui with the Vulkan renderer + GLFW via the renderer's existing deferred-overlay hook — no renderer changes.
- `SceneOutliner` / `SceneInspector` / `EnvironmentPanel` edit a `SceneFile&` live: select an entity, tune transform (euler rotation) + material, edit sun/fog/clearColor/point lights, and Save back via `saveSceneFile`.
- `games/11-sandbox` is now the editor host: mutable scene, hold-RMB-to-look camera, per-frame render-data re-derive.
- Added `quatToEuler`/`eulerToQuat` (degrees, XYZ) with a round-trip test for usable rotation editing.

## Test plan

- [x] `test_quat_euler` — euler round-trip + identity + known-axis (4 cases)
- [x] Full suite green (44/44)
- [x] sandbox builds clean, links ironcore_editor
- [ ] Visual: select/edit entities + lighting live; hold-RMB fly; Save persists to demo.json

## Known v1 limitations

- Inspect & tune only — no add/delete entity, mesh/texture-path edit, or gizmos (→ M31).
- Single selection; Save overwrites the scene file in place; no undo/redo.
- Editor module is Vulkan-only.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — 44/44 green (43 from M29 + `test_quat_euler`).
- [ ] `ironcore_editor` + `sandbox` build clean; shipping games (net-shooter) do NOT link the editor / ImGui.
- [ ] Visual: panels appear; entity edits + lighting edits are live; hold-RMB flies; Save writes `demo.json`; relaunch shows persisted edits.
- [ ] PR CI green (CI builds the Vulkan config + imgui; no new headless test beyond `test_quat_euler`).
